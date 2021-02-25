/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/db/s/resharding/resharding_donor_service.h"

#include <fmt/format.h>

#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(reshardingDonorFailsBeforePreparingToMirror);

using namespace fmt::literals;

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

ChunkManager getShardedCollectionRoutingInfoWithRefreshAndFlush(const NamespaceString& nss) {
    auto opCtx = cc().makeOperationContext();

    auto swRoutingInfo = Grid::get(opCtx.get())
                             ->catalogCache()
                             ->getShardedCollectionRoutingInfoWithRefresh(opCtx.get(), nss);
    auto routingInfo = uassertStatusOK(swRoutingInfo);

    CatalogCacheLoader::get(opCtx.get()).waitForCollectionFlush(opCtx.get(), nss);

    return routingInfo;
}

void refreshTemporaryReshardingCollection(const ReshardingDonorDocument& donorDoc) {
    auto tempNss =
        constructTemporaryReshardingNss(donorDoc.getNss().db(), donorDoc.getExistingUUID());
    std::ignore = getShardedCollectionRoutingInfoWithRefreshAndFlush(tempNss);
}

Timestamp generateMinFetchTimestamp(const ReshardingDonorDocument& donorDoc) {
    auto opCtx = cc().makeOperationContext();

    // Do a no-op write and use the OpTime as the minFetchTimestamp
    writeConflictRetry(
        opCtx.get(),
        "resharding donor minFetchTimestamp",
        NamespaceString::kRsOplogNamespace.ns(),
        [&] {
            AutoGetDb db(opCtx.get(), donorDoc.getNss().db(), MODE_IX);
            Lock::CollectionLock collLock(opCtx.get(), donorDoc.getNss(), MODE_S);

            AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);

            const std::string msg = str::stream()
                << "All future oplog entries on the namespace " << donorDoc.getNss().ns()
                << " must include a 'destinedRecipient' field";
            WriteUnitOfWork wuow(opCtx.get());
            opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
                opCtx.get(),
                donorDoc.getNss(),
                donorDoc.getExistingUUID(),
                {},
                BSON("msg" << msg),
                boost::none,
                boost::none,
                boost::none,
                boost::none);
            wuow.commit();
        });

    auto generatedOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult result;
    uassertStatusOK(waitForWriteConcern(
        opCtx.get(), generatedOpTime, WriteConcerns::kMajorityWriteConcern, &result));

    return generatedOpTime.getTimestamp();
}

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
}
}  // namespace

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingDonorService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<DonorStateMachine>(std::move(initialState));
}

ReshardingDonorService::DonorStateMachine::DonorStateMachine(const BSONObj& donorDoc)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _donorDoc(ReshardingDonorDocument::parse(IDLParserErrorContext("ReshardingDonorDocument"),
                                               donorDoc)),
      _id(_donorDoc.getCommonReshardingMetadata().get_id()) {}

ReshardingDonorService::DonorStateMachine::~DonorStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allRecipientsDoneCloning.getFuture().isReady());
    invariant(_allRecipientsDoneApplying.getFuture().isReady());
    invariant(_coordinatorHasDecisionPersisted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

SemiFuture<void> ReshardingDonorService::DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(
            [this] { _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(); })
        .then([this, executor] {
            return _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(executor);
        })
        .then([this, executor] {
            return _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(executor);
        })
        .then([this] { _writeTransactionOplogEntryThenTransitionToBlockingWrites(); })
        .then([this, executor] {
            return _awaitCoordinatorHasDecisionPersistedThenTransitionToDropping(executor);
        })
        .then([this] { return _dropOriginalCollection(); })
        .onError([this](Status status) {
            LOGV2(4956400,
                  "Resharding operation donor state machine failed",
                  "namespace"_attr = _donorDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            _transitionStateAndUpdateCoordinator(DonorStateEnum::kError, boost::none, status);

            // TODO SERVER-52838: Ensure all local collections that may have been created for
            // resharding are removed, with the exception of the ReshardingDonorDocument, before
            // transitioning to kDone.
            _transitionStateAndUpdateCoordinator(DonorStateEnum::kDone, boost::none, status);
            return status;
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                if (_completionPromise.getFuture().isReady()) {
                    // interrupt() was called before we got here.
                    return;
                }
            }

            if (status.isOK()) {
                // The shared_ptr stored in the PrimaryOnlyService's map for the
                // ReshardingDonorService Instance is removed when the donor state document tied to
                // the instance is deleted. It is necessary to use shared_from_this() to extend the
                // lifetime so the code can safely finish executing.
                _removeDonorDocument();
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.emplaceValue();
                }
            } else {
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(status);
                }
            }
        })
        .semi();
}

void ReshardingDonorService::DonorStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lk(_mutex);
    _onAbortOrStepdown(lk, status);
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

boost::optional<BSONObj> ReshardingDonorService::DonorStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    ReshardingMetrics::ReporterOptions options(ReshardingMetrics::ReporterOptions::Role::kDonor,
                                               _id,
                                               _donorDoc.getNss(),
                                               _donorDoc.getReshardingKey().toBSON(),
                                               false);
    return ReshardingMetrics::get(cc().getServiceContext())->reportForCurrentOp(options);
}

void ReshardingDonorService::DonorStateMachine::onReshardingFieldsChanges(
    OperationContext* opCtx, const TypeCollectionReshardingFields& reshardingFields) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (reshardingFields.getAbortReason()) {
        auto status = getStatusFromAbortReason(reshardingFields);
        _onAbortOrStepdown(lk, status);
        return;
    }

    auto coordinatorState = reshardingFields.getState();
    if (coordinatorState >= CoordinatorStateEnum::kApplying) {
        ensureFulfilledPromise(lk, _allRecipientsDoneCloning);
    }

    if (coordinatorState >= CoordinatorStateEnum::kBlockingWrites) {
        _critSec.emplace(opCtx->getServiceContext(), _donorDoc.getNss());

        ensureFulfilledPromise(lk, _allRecipientsDoneApplying);
    }

    if (coordinatorState >= CoordinatorStateEnum::kDecisionPersisted) {
        ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
    }

    if (coordinatorState >= CoordinatorStateEnum::kDone) {
        _critSec.reset();
    }
}

void ReshardingDonorService::DonorStateMachine::
    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData() {
    if (_donorDoc.getState() > DonorStateEnum::kPreparingToDonate) {
        invariant(_donorDoc.getMinFetchTimestamp());
        return;
    }

    ReshardingCloneSize cloneSizeEstimate;
    {
        auto opCtx = cc().makeOperationContext();
        auto rawOpCtx = opCtx.get();
        const auto shardId = ShardingState::get(rawOpCtx)->shardId();

        const auto& nss = _donorDoc.getNss();
        const auto& nssUUID = _donorDoc.getExistingUUID();
        const auto& reshardingUUID = _donorDoc.get_id();

        AutoGetCollectionForRead coll(rawOpCtx, _donorDoc.getNss());
        if (!coll) {
            cloneSizeEstimate.setBytesToClone(0);
            cloneSizeEstimate.setDocumentsToClone(0);
        } else {
            cloneSizeEstimate.setBytesToClone(coll->dataSize(rawOpCtx));
            cloneSizeEstimate.setDocumentsToClone(coll->numRecords(rawOpCtx));
        }

        LOGV2_DEBUG(5390702,
                    2,
                    "Resharding estimated size",
                    "reshardingUUID"_attr = reshardingUUID,
                    "namespace"_attr = nss,
                    "donorShardId"_attr = shardId,
                    "sizeInfo"_attr = cloneSizeEstimate);

        IndexBuildsCoordinator::get(rawOpCtx)->assertNoIndexBuildInProgForCollection(nssUUID);
    }

    // Recipient shards expect to read from the donor shard's existing sharded collection
    // and the config.cache.chunks collection of the temporary resharding collection using
    // {atClusterTime: <fetchTimestamp>}. Refreshing the temporary resharding collection on
    // the donor shards causes them to create the config.cache.chunks collection. Without
    // this refresh, the {atClusterTime: <fetchTimestamp>} read on the config.cache.chunks
    // namespace would fail with a SnapshotUnavailable error response.
    refreshTemporaryReshardingCollection(_donorDoc);

    auto minFetchTimestamp = generateMinFetchTimestamp(_donorDoc);
    _transitionStateAndUpdateCoordinator(
        DonorStateEnum::kDonatingInitialData, minFetchTimestamp, boost::none, cloneSizeEstimate);
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonatingInitialData) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneCloning.getFuture()
        .thenRunOn(**executor)
        .then([this]() { _transitionState(DonorStateEnum::kDonatingOplogEntries); })
        .onCompletion([=](Status s) {
            if (MONGO_unlikely(reshardingDonorFailsBeforePreparingToMirror.shouldFail())) {
                uasserted(ErrorCodes::InternalError, "Failing for test");
            }
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kDonatingOplogEntries) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _allRecipientsDoneApplying.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kPreparingToBlockWrites);
    });
}

void ReshardingDonorService::DonorStateMachine::
    _writeTransactionOplogEntryThenTransitionToBlockingWrites() {
    if (_donorDoc.getState() > DonorStateEnum::kPreparingToBlockWrites) {
        return;
    }

    {
        const auto& nss = _donorDoc.getNss();
        const auto& nssUUID = _donorDoc.getExistingUUID();
        const auto& reshardingUUID = _donorDoc.get_id();
        auto opCtx = cc().makeOperationContext();
        auto rawOpCtx = opCtx.get();

        auto generateOplogEntry = [&](ShardId destinedRecipient) {
            repl::MutableOplogEntry oplog;
            oplog.setNss(nss);
            oplog.setOpType(repl::OpTypeEnum::kNoop);
            oplog.setUuid(nssUUID);
            oplog.setDestinedRecipient(destinedRecipient);
            oplog.setObject(
                BSON("msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                          nss.toString())));
            oplog.setObject2(
                BSON("type" << kReshardFinalOpLogType << "reshardingUUID" << reshardingUUID));
            oplog.setOpTime(OplogSlot());
            oplog.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
            return oplog;
        };

        try {
            Timer latency;

            const auto recipients = getRecipientShards(rawOpCtx, nss, nssUUID);

            for (const auto& recipient : recipients) {
                auto oplog = generateOplogEntry(recipient);
                writeConflictRetry(
                    rawOpCtx,
                    "ReshardingBlockWritesOplog",
                    NamespaceString::kRsOplogNamespace.ns(),
                    [&] {
                        AutoGetOplog oplogWrite(rawOpCtx, OplogAccessMode::kWrite);
                        WriteUnitOfWork wunit(rawOpCtx);
                        const auto& oplogOpTime = repl::logOp(rawOpCtx, &oplog);
                        uassert(5279507,
                                str::stream()
                                    << "Failed to create new oplog entry for oplog with opTime: "
                                    << oplog.getOpTime().toString() << ": "
                                    << redact(oplog.toBSON()),
                                !oplogOpTime.isNull());
                        wunit.commit();
                    });
            }

            {
                stdx::lock_guard<Latch> lg(_mutex);
                LOGV2_DEBUG(5279504,
                            0,
                            "Committed oplog entries to temporarily block writes for resharding",
                            "namespace"_attr = nss,
                            "reshardingUUID"_attr = reshardingUUID,
                            "numRecipients"_attr = recipients.size(),
                            "duration"_attr = duration_cast<Milliseconds>(latency.elapsed()));
                ensureFulfilledPromise(lg, _finalOplogEntriesWritten);
            }
        } catch (const DBException& e) {
            const auto& status = e.toStatus();
            stdx::lock_guard<Latch> lg(_mutex);
            LOGV2_ERROR(5279508,
                        "Exception while writing resharding final oplog entries",
                        "reshardingUUID"_attr = reshardingUUID,
                        "error"_attr = status);
            ensureFulfilledPromise(lg, _finalOplogEntriesWritten, status);
            uassertStatusOK(status);
        }
    }

    _transitionState(DonorStateEnum::kBlockingWrites);
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitFinalOplogEntriesWritten() {
    return _finalOplogEntriesWritten.getFuture();
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitCoordinatorHasDecisionPersistedThenTransitionToDropping(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorDoc.getState() > DonorStateEnum::kBlockingWrites) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasDecisionPersisted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(DonorStateEnum::kDropping);
    });
}

void ReshardingDonorService::DonorStateMachine::_dropOriginalCollection() {
    if (_donorDoc.getState() > DonorStateEnum::kDropping) {
        return;
    }

    {
        auto opCtx = cc().makeOperationContext();
        resharding::data_copy::ensureCollectionDropped(
            opCtx.get(), _donorDoc.getNss(), _donorDoc.getExistingUUID());
    }

    _transitionStateAndUpdateCoordinator(DonorStateEnum::kDone);
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorStateEnum endState,
    boost::optional<Timestamp> minFetchTimestamp,
    boost::optional<Status> abortReason) {
    ReshardingDonorDocument replacementDoc(_donorDoc);
    replacementDoc.setState(endState);

    emplaceMinFetchTimestampIfExists(replacementDoc, minFetchTimestamp);
    emplaceAbortReasonIfExists(replacementDoc, abortReason);

    // For logging purposes.
    auto oldState = _donorDoc.getState();
    auto newState = replacementDoc.getState();

    _updateDonorDocument(std::move(replacementDoc));

    LOGV2_INFO(5279505,
               "Transitioned resharding donor state",
               "newState"_attr = DonorState_serializer(newState),
               "oldState"_attr = DonorState_serializer(oldState),
               "ns"_attr = _donorDoc.getNss(),
               "collectionUUID"_attr = _donorDoc.getExistingUUID(),
               "reshardingUUID"_attr = _donorDoc.get_id());
}

void ReshardingDonorService::DonorStateMachine::_transitionStateAndUpdateCoordinator(
    DonorStateEnum endState,
    boost::optional<Timestamp> minFetchTimestamp,
    boost::optional<Status> abortReason,
    boost::optional<ReshardingCloneSize> cloneSizeEstimate) {
    _transitionState(endState, minFetchTimestamp, abortReason);

    auto opCtx = cc().makeOperationContext();
    auto shardId = ShardingState::get(opCtx.get())->shardId();

    BSONObjBuilder updateBuilder;
    updateBuilder.append("donorShards.$.state", DonorState_serializer(endState));

    if (minFetchTimestamp) {
        updateBuilder.append("donorShards.$.minFetchTimestamp", minFetchTimestamp.get());
    }

    if (abortReason) {
        BSONObjBuilder abortReasonBuilder;
        abortReason.get().serializeErrorToBSON(&abortReasonBuilder);
        updateBuilder.append("donorShards.$.abortReason", abortReasonBuilder.obj());
    }

    if (cloneSizeEstimate) {
        updateBuilder.append("donorShards.$.cloneSizeInfo", cloneSizeEstimate.get().toBSON());
    }

    uassertStatusOK(
        Grid::get(opCtx.get())
            ->catalogClient()
            ->updateConfigDocument(opCtx.get(),
                                   NamespaceString::kConfigReshardingOperationsNamespace,
                                   BSON("_id" << _donorDoc.get_id() << "donorShards.id" << shardId),
                                   BSON("$set" << updateBuilder.done()),
                                   false /* upsert */,
                                   ShardingCatalogClient::kMajorityWriteConcern));
}

void ReshardingDonorService::DonorStateMachine::insertStateDocument(
    OperationContext* opCtx, const ReshardingDonorDocument& donorDoc) {
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.add(opCtx, donorDoc, kNoWaitWriteConcern);
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(
    ReshardingDonorDocument&& replacementDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.update(opCtx.get(),
                 BSON(ReshardingDonorDocument::k_idFieldName << _id),
                 replacementDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _donorDoc = replacementDoc;
}

void ReshardingDonorService::DonorStateMachine::_removeDonorDocument() {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.remove(opCtx.get(),
                 BSON(ReshardingDonorDocument::k_idFieldName << _id),
                 WriteConcerns::kMajorityWriteConcern);
    _donorDoc = {};
}

void ReshardingDonorService::DonorStateMachine::_onAbortOrStepdown(WithLock, Status status) {
    if (!_allRecipientsDoneCloning.getFuture().isReady()) {
        _allRecipientsDoneCloning.setError(status);
    }

    if (!_allRecipientsDoneApplying.getFuture().isReady()) {
        _allRecipientsDoneApplying.setError(status);
    }

    if (!_finalOplogEntriesWritten.getFuture().isReady()) {
        _finalOplogEntriesWritten.setError(status);
    }

    if (!_coordinatorHasDecisionPersisted.getFuture().isReady()) {
        _coordinatorHasDecisionPersisted.setError(status);
    }
}

}  // namespace mongo

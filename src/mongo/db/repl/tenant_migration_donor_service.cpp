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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/repl/tenant_migration_donor_service.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/cancelation.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterPersistingInitialDonorStateDoc);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeLeavingDataSyncState);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationBeforeEnteringFutureChain);

const std::string kTTLIndexName = "TenantMigrationDonorTTLIndex";
const std::string kExternalKeysTTLIndexName = "ExternalKeysTTLIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly);

const int kMaxRecipientKeyDocsFindAttempts = 10;

bool shouldStopCreatingTTLIndex(Status status, const CancelationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopInsertingDonorStateDoc(Status status, const CancelationToken& token) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress ||
        token.isCanceled();
}

bool shouldStopUpdatingDonorStateDoc(Status status, const CancelationToken& token) {
    return status.isOK() || token.isCanceled();
}

bool shouldStopSendingRecipientCommand(Status status, const CancelationToken& token) {
    return status.isOK() ||
        !(ErrorCodes::isRetriableError(status) ||
          status == ErrorCodes::FailedToSatisfyReadPreference) ||
        token.isCanceled();
}

bool shouldStopFetchingRecipientClusterTimeKeyDocs(Status status, const CancelationToken& token) {
    // TODO (SERVER-54926): Convert HostUnreachable error in
    // _fetchAndStoreRecipientClusterTimeKeyDocs to specific error.
    return status.isOK() || !ErrorCodes::isRetriableError(status) ||
        status.code() == ErrorCodes::HostUnreachable || token.isCanceled();
}

void checkIfReceivedDonorAbortMigration(const CancelationToken& serviceToken,
                                        const CancelationToken& instanceToken) {
    // If only the instance token was canceled, then we must have gotten donorAbortMigration.
    uassert(ErrorCodes::TenantMigrationAborted,
            "Migration aborted due to receiving donorAbortMigration.",
            !instanceToken.isCanceled() || serviceToken.isCanceled());
}

}  // namespace

// Note this index is required on both the donor and recipient in a tenant migration, since each
// will copy cluster time keys from the other. The donor service is set up on all mongods on stepup
// to primary, so this index will be created on both donors and recipients.
ExecutorFuture<void> TenantMigrationDonorService::createStateDocumentTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("expireAt" << 1) << "name" << kTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopCreatingTTLIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::createExternalKeysTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    return AsyncTry([this] {
               const auto nss = NamespaceString::kExternalKeysCollectionNamespace;

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("ttlExpiresAt" << 1) << "name"
                                                 << kExternalKeysTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([token](Status status) { return shouldStopCreatingTTLIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    return createStateDocumentTTLIndex(executor, token).then([this, executor, token] {
        return createExternalKeysTTLIndex(executor, token);
    });
}

TenantMigrationDonorService::Instance::Instance(ServiceContext* const serviceContext,
                                                const TenantMigrationDonorService* donorService,
                                                const BSONObj& initialState)
    : repl::PrimaryOnlyService::TypedInstance<Instance>(),
      _serviceContext(serviceContext),
      _donorService(donorService),
      _stateDoc(tenant_migration_access_blocker::parseDonorStateDocument(initialState)),
      _instanceName(kServiceName + "-" + _stateDoc.getTenantId()),
      _recipientUri(
          uassertStatusOK(MongoURI::parse(_stateDoc.getRecipientConnectionString().toString()))),
      _sslMode(repl::tenantMigrationDisableX509Auth ? transport::kGlobalSSLMode
                                                    : transport::kEnableSSL) {
    _recipientCmdExecutor = _makeRecipientCmdExecutor();
    _recipientCmdExecutor->startup();

    if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
        // The migration was resumed on stepup.
        stdx::lock_guard<Latch> lg(_mutex);

        _durableState.state = _stateDoc.getState();
        if (_stateDoc.getAbortReason()) {
            _durableState.abortReason =
                getStatusFromCommandResult(_stateDoc.getAbortReason().get());
        }

        _initialDonorStateDurablePromise.emplaceValue();

        if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted ||
            _stateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted) {
            _decisionPromise.emplaceValue();
        }
    }
}

TenantMigrationDonorService::Instance::~Instance() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_initialDonorStateDurablePromise.getFuture().isReady());
    invariant(_receiveDonorForgetMigrationPromise.getFuture().isReady());

    // Unlike the TenantMigrationDonorService's scoped task executor which is shut down on stepdown
    // and joined on stepup, _recipientCmdExecutor is only shut down and joined when the Instance
    // is destroyed. This is safe since ThreadPoolTaskExecutor::shutdown() only cancels the
    // outstanding work on the task executor which the cancelation token will already do, and the
    // Instance will be destroyed on stepup so this is equivalent to joining the task executor on
    // stepup.
    _recipientCmdExecutor->shutdown();
    _recipientCmdExecutor->join();
}

std::shared_ptr<executor::ThreadPoolTaskExecutor>
TenantMigrationDonorService::Instance::_makeRecipientCmdExecutor() {
    ThreadPool::Options threadPoolOptions(_getRecipientCmdThreadPoolLimits());
    threadPoolOptions.threadNamePrefix = _instanceName + "-";
    threadPoolOptions.poolName = _instanceName + "ThreadPool";
    threadPoolOptions.onCreateThread = [this](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        auto client = Client::getCurrent();
        AuthorizationSession::get(*client)->grantInternalAuthorization(&cc());

        // Ideally, we should also associate the client created by _recipientCmdExecutor with the
        // TenantMigrationDonorService to make the opCtxs created by the task executor get
        // registered in the TenantMigrationDonorService, and killed on stepdown. But that would
        // require passing the pointer to the TenantMigrationService into the Instance and making
        // constructInstance not const so we can set the client's decoration here. Right now there
        // is no need for that since the task executor is only used with scheduleRemoteCommand and
        // no opCtx will be created (the cancelation token is responsible for canceling the
        // outstanding work on the task executor).
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationKillableByStepdown(lk);
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();

    auto connPoolOptions = executor::ConnectionPool::Options();
    auto donorCertificate = _stateDoc.getDonorCertificateForRecipient();
    auto recipientCertificate = _stateDoc.getRecipientCertificateForDonor();
    if (donorCertificate) {
        invariant(!repl::tenantMigrationDisableX509Auth);
        invariant(recipientCertificate);
        invariant(_sslMode == transport::kEnableSSL);
#ifdef MONGO_CONFIG_SSL
        uassert(ErrorCodes::IllegalOperation,
                "Cannot run tenant migration with x509 authentication as SSL is not enabled",
                getSSLGlobalParams().sslMode.load() != SSLParams::SSLMode_disabled);
        auto donorSSLClusterPEMPayload = donorCertificate->getCertificate().toString() + "\n" +
            donorCertificate->getPrivateKey().toString();
        connPoolOptions.transientSSLParams = TransientSSLParams{
            _recipientUri.connectionString(), std::move(donorSSLClusterPEMPayload)};
#else
        // If SSL is not supported, the donorStartMigration command should have failed certificate
        // field validation.
        MONGO_UNREACHABLE;
#endif
    } else {
        invariant(repl::tenantMigrationDisableX509Auth);
        invariant(!recipientCertificate);
        invariant(_sslMode == transport::kGlobalSSLMode);
    }

    return std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(
            _instanceName + "-Network", nullptr, std::move(hookList), connPoolOptions));
}

boost::optional<BSONObj> TenantMigrationDonorService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    // Ignore connMode and sessionMode because tenant migrations are not associated with
    // sessions and they run in a background thread pool.
    BSONObjBuilder bob;
    bob.append("desc", "tenant donor migration");
    bob.append("migrationCompleted", _completionPromise.getFuture().isReady());
    bob.append("receivedCancelation", _abortMigrationSource.token().isCanceled());
    bob.append("instanceID", _stateDoc.getId().toBSON());
    bob.append("tenantId", _stateDoc.getTenantId());
    bob.append("recipientConnectionString", _stateDoc.getRecipientConnectionString());
    bob.append("readPreference", _stateDoc.getReadPreference().toInnerBSON());
    bob.append("lastDurableState", _durableState.state);
    if (_stateDoc.getMigrationStart()) {
        bob.appendDate("migrationStart", *_stateDoc.getMigrationStart());
    }
    if (_stateDoc.getExpireAt()) {
        bob.appendDate("expireAt", *_stateDoc.getExpireAt());
    }
    if (_stateDoc.getStartMigrationDonorTimestamp()) {
        bob.append("startMigrationDonorTimestamp",
                   _stateDoc.getStartMigrationDonorTimestamp()->toBSON());
    }
    if (_stateDoc.getBlockTimestamp()) {
        bob.append("blockTimestamp", _stateDoc.getBlockTimestamp()->toBSON());
    }
    if (_stateDoc.getCommitOrAbortOpTime()) {
        bob.append("commitOrAbortOpTime", _stateDoc.getCommitOrAbortOpTime()->toBSON());
    }
    if (_stateDoc.getAbortReason()) {
        bob.append("abortReason", _stateDoc.getAbortReason()->toString());
    }
    return bob.obj();
}

Status TenantMigrationDonorService::Instance::checkIfOptionsConflict(
    const TenantMigrationDonorDocument& stateDoc) {
    if (stateDoc.getId() != _stateDoc.getId() ||
        stateDoc.getTenantId() != _stateDoc.getTenantId() ||
        stateDoc.getRecipientConnectionString() != _stateDoc.getRecipientConnectionString() ||
        SimpleBSONObjComparator::kInstance.compare(stateDoc.getReadPreference().toInnerBSON(),
                                                   _stateDoc.getReadPreference().toInnerBSON()) !=
            0) {
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      str::stream()
                          << "Found active migration for tenantId \"" << stateDoc.getTenantId()
                          << "\" with different options " << _stateDoc.toBSON());
    }

    return Status::OK();
}

TenantMigrationDonorService::Instance::DurableState
TenantMigrationDonorService::Instance::getDurableState(OperationContext* opCtx) {
    // Wait for the insert of the state doc to become majority-committed.
    _initialDonorStateDurablePromise.getFuture().get(opCtx);

    stdx::lock_guard<Latch> lg(_mutex);
    return _durableState;
}

void TenantMigrationDonorService::Instance::onReceiveDonorAbortMigration() {
    _abortMigrationSource.cancel();

    stdx::lock_guard<Latch> lg(_mutex);
    if (auto fetcher = _recipientKeysFetcher.lock()) {
        fetcher->shutdown();
    }
}

void TenantMigrationDonorService::Instance::onReceiveDonorForgetMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_receiveDonorForgetMigrationPromise.getFuture().isReady()) {
        _receiveDonorForgetMigrationPromise.emplaceValue();
    }
}

void TenantMigrationDonorService::Instance::interrupt(Status status) {
    stdx::lock_guard<Latch> lg(_mutex);
    // Resolve any unresolved promises to avoid hanging.
    if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
        _initialDonorStateDurablePromise.setError(status);
    }
    if (!_receiveDonorForgetMigrationPromise.getFuture().isReady()) {
        _receiveDonorForgetMigrationPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
    if (!_decisionPromise.getFuture().isReady()) {
        _decisionPromise.setError(status);
    }
}

ExecutorFuture<void>
TenantMigrationDonorService::Instance::_fetchAndStoreRecipientClusterTimeKeyDocs(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancelationToken& serviceToken,
    const CancelationToken& instanceToken) {

    return AsyncTry([this,
                     self = shared_from_this(),
                     executor,
                     recipientTargeterRS,
                     serviceToken,
                     instanceToken] {
               return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, instanceToken)
                   .thenRunOn(**executor)
                   .then([this, self = shared_from_this(), executor](HostAndPort host) {
                       const auto nss = NamespaceString::kKeysCollectionNamespace;

                       const auto cmdObj = [&] {
                           FindCommand request(NamespaceStringOrUUID{nss});
                           request.setReadConcern(
                               repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern)
                                   .toBSONInner());
                           return request.toBSON(BSONObj());
                       }();

                       std::vector<ExternalKeysCollectionDocument> keyDocs;
                       boost::optional<Status> fetchStatus;

                       auto fetcherCallback =
                           [this, self = shared_from_this(), &keyDocs, &fetchStatus](
                               const Fetcher::QueryResponseStatus& dataStatus,
                               Fetcher::NextAction* nextAction,
                               BSONObjBuilder* getMoreBob) {
                               // Throw out any accumulated results on error
                               if (!dataStatus.isOK()) {
                                   fetchStatus = dataStatus.getStatus();
                                   keyDocs.clear();
                                   return;
                               }

                               const auto& data = dataStatus.getValue();
                               for (const BSONObj& doc : data.documents) {
                                   keyDocs.push_back(
                                       tenant_migration_util::makeExternalClusterTimeKeyDoc(
                                           _stateDoc.getId(), doc.getOwned()));
                               }
                               fetchStatus = Status::OK();

                               if (!getMoreBob) {
                                   return;
                               }
                               getMoreBob->append("getMore", data.cursorId);
                               getMoreBob->append("collection", data.nss.coll());
                           };

                       auto fetcher = std::make_shared<Fetcher>(
                           _recipientCmdExecutor.get(),
                           host,
                           nss.db().toString(),
                           cmdObj,
                           fetcherCallback,
                           kPrimaryOnlyReadPreference.toContainingBSON(),
                           executor::RemoteCommandRequest::kNoTimeout, /* findNetworkTimeout */
                           executor::RemoteCommandRequest::kNoTimeout, /* getMoreNetworkTimeout */
                           RemoteCommandRetryScheduler::makeRetryPolicy<
                               ErrorCategory::RetriableError>(
                               kMaxRecipientKeyDocsFindAttempts,
                               executor::RemoteCommandRequest::kNoTimeout),
                           _sslMode);
                       uassertStatusOK(fetcher->schedule());

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           _recipientKeysFetcher = fetcher;
                       }

                       fetcher->join();

                       {
                           stdx::lock_guard<Latch> lg(_mutex);
                           _recipientKeysFetcher.reset();
                       }

                       if (!fetchStatus) {
                           // The callback never got invoked.
                           uasserted(5340400, "Internal error running cursor callback in command");
                       }
                       uassertStatusOK(fetchStatus.get());

                       return keyDocs;
                   })
                   .then([this, self = shared_from_this(), executor, serviceToken, instanceToken](
                             auto keyDocs) {
                       checkIfReceivedDonorAbortMigration(serviceToken, instanceToken);

                       tenant_migration_util::storeExternalClusterTimeKeyDocs(executor,
                                                                              std::move(keyDocs));
                   });
           })
        .until([instanceToken](Status status) {
            return shouldStopFetchingRecipientClusterTimeKeyDocs(status, instanceToken);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_insertStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    invariant(_stateDoc.getState() == TenantMigrationDonorStateEnum::kUninitialized);
    _stateDoc.setState(TenantMigrationDonorStateEnum::kAbortingIndexBuilds);

    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId());
                       const auto updateMod = BSON("$setOnInsert" << _stateDoc.toBSON());
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       // '$setOnInsert' update operator can never modify an existing on-disk state
                       // doc.
                       invariant(!updateResult.numDocsModified);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<repl::OpTime> TenantMigrationDonorService::Instance::_updateStateDoc(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const TenantMigrationDonorStateEnum nextState,
    const CancelationToken& token) {
    const auto originalStateDocBson = _stateDoc.toBSON();

    return AsyncTry([this, self = shared_from_this(), executor, nextState, originalStateDocBson] {
               boost::optional<repl::OpTime> updateOpTime;

               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "TenantMigrationDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       WriteUnitOfWork wuow(opCtx);

                       const auto originalRecordId = Helpers::findOne(opCtx,
                                                                      collection.getCollection(),
                                                                      originalStateDocBson,
                                                                      false /* requireIndex */);
                       const auto originalSnapshot = Snapshotted<BSONObj>(
                           opCtx->recoveryUnit()->getSnapshotId(), originalStateDocBson);
                       invariant(!originalRecordId.isNull());

                       // Reserve an opTime for the write.
                       auto oplogSlot =
                           repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

                       // Update the state.
                       _stateDoc.setState(nextState);
                       switch (nextState) {
                           case TenantMigrationDonorStateEnum::kDataSync: {
                               _stateDoc.setStartMigrationDonorTimestamp(oplogSlot.getTimestamp());
                               break;
                           }
                           case TenantMigrationDonorStateEnum::kBlocking: {
                               _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());

                               auto mtab = tenant_migration_access_blocker::
                                   getTenantMigrationDonorAccessBlocker(_serviceContext,
                                                                        _stateDoc.getTenantId());
                               invariant(mtab);

                               mtab->startBlockingWrites();
                               opCtx->recoveryUnit()->onRollback(
                                   [mtab] { mtab->rollBackStartBlocking(); });
                               break;
                           }
                           case TenantMigrationDonorStateEnum::kCommitted:
                               _stateDoc.setCommitOrAbortOpTime(oplogSlot);
                               break;
                           case TenantMigrationDonorStateEnum::kAborted: {
                               _stateDoc.setCommitOrAbortOpTime(oplogSlot);

                               invariant(_abortReason);
                               BSONObjBuilder bob;
                               _abortReason.get().serializeErrorToBSON(&bob);
                               _stateDoc.setAbortReason(bob.obj());
                               break;
                           }
                           default:
                               MONGO_UNREACHABLE;
                       }
                       const auto updatedStateDocBson = _stateDoc.toBSON();

                       CollectionUpdateArgs args;
                       args.criteria = BSON("_id" << _stateDoc.getId());
                       args.oplogSlot = oplogSlot;
                       args.update = updatedStateDocBson;

                       collection->updateDocument(opCtx,
                                                  originalRecordId,
                                                  originalSnapshot,
                                                  updatedStateDocBson,
                                                  false,
                                                  nullptr /* OpDebug* */,
                                                  &args);

                       wuow.commit();

                       updateOpTime = oplogSlot;
                   });

               invariant(updateOpTime);
               return updateOpTime.get();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<repl::OpTime>
TenantMigrationDonorService::Instance::_markStateDocAsGarbageCollectable(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancelationToken& token) {
    _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                          Milliseconds{repl::tenantMigrationGarbageCollectionDelayMS.load()});
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable.pauseWhileSet(opCtx);

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx,
                   "TenantMigrationDonorMarkStateDocAsGarbageCollectable",
                   _stateDocumentsNS.ns(),
                   [&] {
                       const auto filter =
                           BSON(TenantMigrationDonorDocument::kIdFieldName << _stateDoc.getId());
                       const auto updateMod = _stateDoc.toBSON();
                       auto updateResult = Helpers::upsert(
                           opCtx, _stateDocumentsNS.ns(), filter, updateMod, /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([token](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopUpdatingDonorStateDoc(swOpTime.getStatus(), token);
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancelationToken::uncancelable());
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_waitForMajorityWriteConcern(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, repl::OpTime opTime) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime))
        .thenRunOn(**executor)
        .then([this, self = shared_from_this()] {
            stdx::lock_guard<Latch> lg(_mutex);
            _durableState.state = _stateDoc.getState();
            switch (_durableState.state) {
                case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
                    if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
                        _initialDonorStateDurablePromise.emplaceValue();
                    }
                    break;
                case TenantMigrationDonorStateEnum::kDataSync:
                case TenantMigrationDonorStateEnum::kBlocking:
                case TenantMigrationDonorStateEnum::kCommitted:
                    break;
                case TenantMigrationDonorStateEnum::kAborted:
                    invariant(_abortReason);
                    _durableState.abortReason = _abortReason;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        });
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendCommandToRecipient(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const BSONObj& cmdObj,
    const CancelationToken& token) {
    return AsyncTry(
               [this, self = shared_from_this(), executor, recipientTargeterRS, cmdObj, token] {
                   return recipientTargeterRS->findHost(kPrimaryOnlyReadPreference, token)
                       .thenRunOn(**executor)
                       .then([this, self = shared_from_this(), executor, cmdObj, token](
                                 auto recipientHost) {
                           executor::RemoteCommandRequest request(
                               std::move(recipientHost),
                               NamespaceString::kAdminDb.toString(),
                               std::move(cmdObj),
                               rpc::makeEmptyMetadata(),
                               nullptr);
                           request.sslMode = _sslMode;

                           return (_recipientCmdExecutor)
                               ->scheduleRemoteCommand(std::move(request), token)
                               .then([this,
                                      self = shared_from_this()](const auto& response) -> Status {
                                   if (!response.isOK()) {
                                       return response.status;
                                   }
                                   auto commandStatus = getStatusFromCommandResult(response.data);
                                   commandStatus.addContext(
                                       "Tenant migration recipient command failed");
                                   return commandStatus;
                               });
                       });
               })
        .until([token](Status status) { return shouldStopSendingRecipientCommand(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientSyncDataCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancelationToken& token) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    const auto cmdObj = [&] {
        auto donorConnString =
            repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();

        RecipientSyncData request;
        request.setDbName(NamespaceString::kAdminDb);

        MigrationRecipientCommonData commonData(_stateDoc.getId(),
                                                donorConnString.toString(),
                                                _stateDoc.getTenantId().toString(),
                                                _stateDoc.getReadPreference());
        commonData.setRecipientCertificateForDonor(_stateDoc.getRecipientCertificateForDonor());
        request.setMigrationRecipientCommonData(commonData);
        invariant(_stateDoc.getStartMigrationDonorTimestamp());
        request.setStartMigrationDonorTimestamp(*_stateDoc.getStartMigrationDonorTimestamp());
        request.setReturnAfterReachingDonorTimestamp(_stateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }();

    return _sendCommandToRecipient(executor, recipientTargeterRS, cmdObj, token);
}

ExecutorFuture<void> TenantMigrationDonorService::Instance::_sendRecipientForgetMigrationCommand(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    std::shared_ptr<RemoteCommandTargeter> recipientTargeterRS,
    const CancelationToken& token) {

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    auto donorConnString =
        repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();

    RecipientForgetMigration request;
    request.setDbName(NamespaceString::kAdminDb);

    MigrationRecipientCommonData commonData(_stateDoc.getId(),
                                            donorConnString.toString(),
                                            _stateDoc.getTenantId().toString(),
                                            _stateDoc.getReadPreference());
    commonData.setRecipientCertificateForDonor(_stateDoc.getRecipientCertificateForDonor());
    request.setMigrationRecipientCommonData(commonData);

    return _sendCommandToRecipient(executor, recipientTargeterRS, request.toBSON(BSONObj()), token);
}

SemiFuture<void> TenantMigrationDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& serviceToken) noexcept {
    if (!_stateDoc.getMigrationStart()) {
        _stateDoc.setMigrationStart(_serviceContext->getFastClockSource()->now());
    }

    pauseTenantMigrationBeforeEnteringFutureChain.pauseWhileSet();

    _abortMigrationSource = CancelationSource(serviceToken);
    auto recipientTargeterRS = std::make_shared<RemoteCommandTargeterRS>(
        _recipientUri.getSetName(), _recipientUri.getServers());

    return ExecutorFuture<void>(**executor)
        .then([this, self = shared_from_this(), executor, serviceToken] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kUninitialized) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            // Enter "abortingIndexBuilds" state.
            return _insertStateDoc(executor, _abortMigrationSource.token())
                .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                    // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                    // use its base PrimaryOnlyService's cancelation source to pass tokens
                    // in calls to WaitForMajorityService::waitUntilMajority.
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                })
                .then([this, self = shared_from_this()] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();
                    pauseTenantMigrationAfterPersistingInitialDonorStateDoc.pauseWhileSet(opCtx);
                });
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());

            return _fetchAndStoreRecipientClusterTimeKeyDocs(
                executor, recipientTargeterRS, serviceToken, _abortMigrationSource.token());
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kAbortingIndexBuilds) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());

            // Before starting data sync, abort any in-progress index builds.  No new index
            // builds can start while we are doing this because the mtab prevents it.
            {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                auto* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx);
                indexBuildsCoordinator->abortTenantIndexBuilds(
                    opCtx, _stateDoc.getTenantId(), "tenant migration");
                pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState.pauseWhileSet(opCtx);
            }

            // Enter "dataSync" state.
            return _updateStateDoc(executor,
                                   TenantMigrationDonorStateEnum::kDataSync,
                                   _abortMigrationSource.token())

                .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                    // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                    // use its base PrimaryOnlyService's cancelation source to pass tokens
                    // in calls to WaitForMajorityService::waitUntilMajority.
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                });
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kDataSync) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());
            return _sendRecipientSyncDataCommand(
                       executor, recipientTargeterRS, _abortMigrationSource.token())
                .then([this, self = shared_from_this()] {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();
                    pauseTenantMigrationBeforeLeavingDataSyncState.pauseWhileSet(opCtx);
                })
                .then([this, self = shared_from_this(), executor, serviceToken] {
                    checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());

                    // Enter "blocking" state.
                    return _updateStateDoc(executor,
                                           TenantMigrationDonorStateEnum::kBlocking,
                                           _abortMigrationSource.token())
                        .then([this, self = shared_from_this(), executor, serviceToken](
                                  repl::OpTime opTime) {
                            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                            // use its base PrimaryOnlyService's cancelation source to pass tokens
                            // in calls to WaitForMajorityService::waitUntilMajority.
                            checkIfReceivedDonorAbortMigration(serviceToken,
                                                               _abortMigrationSource.token());

                            return _waitForMajorityWriteConcern(executor, std::move(opTime));
                        });
                });
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            if (_stateDoc.getState() > TenantMigrationDonorStateEnum::kBlocking) {
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());

            invariant(_stateDoc.getBlockTimestamp());
            // Source to cancel the timeout if the operation completed in time.
            CancelationSource cancelTimeoutSource;

            auto deadlineReachedFuture = (*executor)->sleepFor(
                Milliseconds(repl::tenantMigrationBlockingStateTimeoutMS.load()),
                cancelTimeoutSource.token());
            std::vector<ExecutorFuture<void>> futures;

            futures.push_back(std::move(deadlineReachedFuture));
            futures.push_back(_sendRecipientSyncDataCommand(
                executor, recipientTargeterRS, _abortMigrationSource.token()));

            return whenAny(std::move(futures))
                .thenRunOn(**executor)
                .then([this, cancelTimeoutSource, self = shared_from_this()](auto result) mutable {
                    const auto& [status, idx] = result;

                    if (idx == 0) {
                        LOGV2(5290301,
                              "Tenant migration blocking stage timeout expired",
                              "timeoutMs"_attr =
                                  repl::tenantMigrationGarbageCollectionDelayMS.load());
                        // Deadline reached, cancel the pending '_sendRecipientSyncDataCommand()'...
                        _abortMigrationSource.cancel();
                        // ...and return error.
                        uasserted(ErrorCodes::ExceededTimeLimit, "Blocking state timeout expired");
                    } else if (idx == 1) {
                        // '_sendRecipientSyncDataCommand()' finished first, cancel the timeout.
                        cancelTimeoutSource.cancel();
                        return status;
                    }
                    MONGO_UNREACHABLE;
                })
                .then([this, self = shared_from_this()]() -> void {
                    auto opCtxHolder = cc().makeOperationContext();
                    auto opCtx = opCtxHolder.get();

                    pauseTenantMigrationBeforeLeavingBlockingState.executeIf(
                        [&](const BSONObj& data) {
                            if (!data.hasField("blockTimeMS")) {
                                pauseTenantMigrationBeforeLeavingBlockingState.pauseWhileSet(opCtx);
                            } else {
                                const auto blockTime =
                                    Milliseconds{data.getIntField("blockTimeMS")};
                                LOGV2(5010400,
                                      "Keep migration in blocking state",
                                      "blockTime"_attr = blockTime);
                                opCtx->sleepFor(blockTime);
                            }
                        },
                        [&](const BSONObj& data) {
                            return !data.hasField("tenantId") ||
                                _stateDoc.getTenantId() == data["tenantId"].str();
                        });

                    if (MONGO_unlikely(
                            abortTenantMigrationBeforeLeavingBlockingState.shouldFail())) {
                        uasserted(ErrorCodes::InternalError, "simulate a tenant migration error");
                    }
                })
                .then([this, self = shared_from_this(), executor, serviceToken] {
                    checkIfReceivedDonorAbortMigration(serviceToken, _abortMigrationSource.token());

                    // Enter "commit" state.
                    return _updateStateDoc(
                               executor, TenantMigrationDonorStateEnum::kCommitted, serviceToken)
                        .then([this, self = shared_from_this(), executor, serviceToken](
                                  repl::OpTime opTime) {
                            // TODO (SERVER-53389): TenantMigration{Donor, Recipient}Service should
                            // use its base PrimaryOnlyService's cancelation source to pass tokens
                            // in calls to WaitForMajorityService::waitUntilMajority.
                            return _waitForMajorityWriteConcern(executor, std::move(opTime))
                                .then([this, self = shared_from_this()] {
                                    // If interrupt is called at some point during execution, it is
                                    // possible that interrupt() will fulfill the promise before we
                                    // do.
                                    if (!_decisionPromise.getFuture().isReady()) {
                                        // Fulfill the promise since we have made a decision.
                                        _decisionPromise.emplaceValue();
                                    }
                                });
                        });
                });
        })
        .onError([this, self = shared_from_this(), executor, serviceToken](Status status) {
            if (_stateDoc.getState() == TenantMigrationDonorStateEnum::kAborted) {
                // The migration was resumed on stepup and it was already aborted.
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(
                _serviceContext, _stateDoc.getTenantId());
            if (status == ErrorCodes::ConflictingOperationInProgress || !mtab) {
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_initialDonorStateDurablePromise.getFuture().isReady()) {
                    // Fulfill the promise since the state doc failed to insert.
                    _initialDonorStateDurablePromise.setError(status);
                }

                return ExecutorFuture<void>(**executor, status);
            } else {
                // Enter "abort" state.
                _abortReason.emplace(status);
                return _updateStateDoc(
                           executor, TenantMigrationDonorStateEnum::kAborted, serviceToken)
                    .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                        return _waitForMajorityWriteConcern(executor, std::move(opTime))
                            .then([this, self = shared_from_this()] {
                                // If interrupt is called at some point during execution, it is
                                // possible that interrupt() will fulfill the promise before we do.
                                if (!_decisionPromise.getFuture().isReady()) {
                                    // Fulfill the promise since we have made a decision.
                                    _decisionPromise.emplaceValue();
                                };
                            });
                    });
            }
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            LOGV2(5006601,
                  "Tenant migration completed",
                  "migrationId"_attr = _stateDoc.getId(),
                  "tenantId"_attr = _stateDoc.getTenantId(),
                  "status"_attr = status,
                  "abortReason"_attr = _abortReason);
        })
        .then([this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
            if (_stateDoc.getExpireAt()) {
                // The migration state has already been marked as garbage collectable. Set the
                // donorForgetMigration promise here since the Instance's destructor has an
                // invariant that _receiveDonorForgetMigrationPromise is ready.
                onReceiveDonorForgetMigration();
                return ExecutorFuture<void>(**executor, Status::OK());
            }

            // Wait for the donorForgetMigration command.
            // If donorAbortMigration has already canceled work, the abortMigrationSource would be
            // canceled and continued usage of the source would lead to incorrect behavior. Thus, we
            // need to use the serviceToken after the migration has reached a decision state in
            // order to continue work, such as sending donorForgetMigration, successfully.
            return std::move(_receiveDonorForgetMigrationPromise.getFuture())
                .thenRunOn(**executor)
                .then(
                    [this, self = shared_from_this(), executor, recipientTargeterRS, serviceToken] {
                        return _sendRecipientForgetMigrationCommand(
                            executor, recipientTargeterRS, serviceToken);
                    })
                .then([this, self = shared_from_this(), executor, serviceToken] {
                    // Note marking the keys as garbage collectable is not atomic with marking the
                    // state document garbage collectable, so an interleaved failover can lead the
                    // keys to be deleted before the state document has an expiration date. This is
                    // acceptable because the decision to forget a migration is not reversible.
                    return tenant_migration_util::markExternalKeysAsGarbageCollectable(
                        _serviceContext,
                        executor,
                        _donorService->getInstanceCleanupExecutor(),
                        _stateDoc.getId(),
                        serviceToken);
                })
                .then([this, self = shared_from_this(), executor, serviceToken] {
                    return _markStateDocAsGarbageCollectable(executor, serviceToken);
                })
                .then([this, self = shared_from_this(), executor](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime));
                });
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            LOGV2(4920400,
                  "Marked migration state as garbage collectable",
                  "migrationId"_attr = _stateDoc.getId(),
                  "expireAt"_attr = _stateDoc.getExpireAt(),
                  "status"_attr = status);

            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        })
        .semi();
}

}  // namespace mongo

/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

using RecipientStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<RecipientStateEnum>;
using PauseDuringStateTransitions =
    resharding_service_test_helpers::PauseDuringStateTransitions<RecipientStateEnum>;
using OpObserverForTest =
    resharding_service_test_helpers::OpObserverForTest<RecipientStateEnum,
                                                       ReshardingRecipientDocument>;
const ShardId recipientShardId{"myShardId"};

class ExternalStateForTest : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return recipientShardId;
    }

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override {}

    ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                 const NamespaceString& nss) override {
        invariant(nss == _sourceNss);

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            nss,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion(100, 0, epoch, boost::none /* timestamp */),
            _someDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               std::move(epoch),
                                               boost::none /* timestamp */,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_someDonorId,
                            DatabaseVersion(UUID::gen()),
                            _makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionUUID& uuid,
        Timestamp afterClusterTime,
        StringData reason) override {
        invariant(nss == _sourceNss);
        return {BSONObj(), uuid};
    }

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionUUID& uuid,
        Timestamp afterClusterTime,
        StringData reason) override {
        invariant(nss == _sourceNss);
        return {std::vector<BSONObj>{}, BSONObj()};
    }

    void withShardVersionRetry(OperationContext* opCtx,
                               const NamespaceString& nss,
                               StringData reason,
                               unique_function<void()> callback) override {
        callback();
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {}

private:
    RoutingTableHistoryValueHandle _makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::move(rt), ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "oldKey";

    const NamespaceString _sourceNss{"sourcedb", "sourcecollection"};
    const CollectionUUID _sourceUUID = UUID::gen();

    const ShardId _someDonorId{"myDonorId"};
};

class RecipientOpObserverForTest : public OpObserverForTest {
public:
    RecipientOpObserverForTest(std::shared_ptr<RecipientStateTransitionController> controller)
        : OpObserverForTest(std::move(controller),
                            NamespaceString::kRecipientReshardingOperationsNamespace) {}

    RecipientStateEnum getState(const ReshardingRecipientDocument& recipientDoc) override {
        return recipientDoc.getMutableState().getState();
    }
};

class DataReplicationForTest : public ReshardingDataReplicationInterface {
public:
    SemiFuture<void> runUntilStrictlyConsistent(
        std::shared_ptr<executor::TaskExecutor> executor,
        std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory opCtxFactory,
        const mongo::Date_t& startConfigTxnCloneTime) override {
        return makeReadyFutureWith([] {}).semi();
    };

    void startOplogApplication() override{};

    SharedSemiFuture<void> awaitCloningDone() override {
        return makeReadyFutureWith([] {}).share();
    };

    SharedSemiFuture<void> awaitStrictlyConsistent() override {
        return makeReadyFutureWith([] {}).share();
    };

    void shutdown() override {}
};

class ReshardingRecipientServiceForTest : public ReshardingRecipientService {
public:
    explicit ReshardingRecipientServiceForTest(ServiceContext* serviceContext)
        : ReshardingRecipientService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override {
        return std::make_shared<RecipientStateMachine>(
            this,
            ReshardingRecipientDocument::parse({"ReshardingRecipientServiceForTest"}, initialState),
            std::make_unique<ExternalStateForTest>(),
            [](auto...) { return std::make_unique<DataReplicationForTest>(); });
    }
};

/**
 * Tests the behavior of the ReshardingRecipientService upon recovery from failover.
 */
class ReshardingRecipientServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ReshardingRecipientServiceForTest>(serviceContext);
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        auto serviceContext = getServiceContext();
        auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
        repl::DropPendingCollectionReaper::set(
            serviceContext, std::make_unique<repl::DropPendingCollectionReaper>(storageMock.get()));
        repl::StorageInterface::set(serviceContext, std::move(storageMock));

        _controller = std::make_shared<RecipientStateTransitionController>();
        _opObserverRegistry->addObserver(std::make_unique<RecipientOpObserverForTest>(_controller));
    }

    RecipientStateTransitionController* controller() {
        return _controller.get();
    }

    ReshardingRecipientDocument makeStateDocument(bool isAlsoDonor) {
        RecipientShardContext recipientCtx;
        recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

        ReshardingRecipientDocument doc(std::move(recipientCtx),
                                        {ShardId{"donor1"},
                                         isAlsoDonor ? recipientShardId : ShardId{"donor2"},
                                         ShardId{"donor3"}},
                                        durationCount<Milliseconds>(Milliseconds{5}));

        NamespaceString sourceNss("sourcedb", "sourcecollection");
        auto sourceUUID = UUID::gen();
        auto commonMetadata =
            CommonReshardingMetadata(UUID::gen(),
                                     sourceNss,
                                     sourceUUID,
                                     constructTemporaryReshardingNss(sourceNss.db(), sourceUUID),
                                     BSON("newKey" << 1));

        doc.setCommonReshardingMetadata(std::move(commonMetadata));
        return doc;
    }

    void createSourceCollection(OperationContext* opCtx,
                                const ReshardingRecipientDocument& recipientDoc) {
        CollectionOptions options;
        options.uuid = recipientDoc.getSourceUUID();
        resharding::data_copy::ensureCollectionDropped(opCtx, recipientDoc.getSourceNss());
        resharding::data_copy::ensureCollectionExists(opCtx, recipientDoc.getSourceNss(), options);
    }

    void notifyToStartCloning(OperationContext* opCtx,
                              RecipientStateMachine& recipient,
                              const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCloning);
    }

    void notifyReshardingCommitting(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(
            opCtx, recipient, recipientDoc, CoordinatorStateEnum::kCommitting);
    }

    void notifyReshardingAborting(OperationContext* opCtx,
                                  RecipientStateMachine& recipient,
                                  const ReshardingRecipientDocument& recipientDoc) {
        _onReshardingFieldsChanges(opCtx, recipient, recipientDoc, CoordinatorStateEnum::kAborting);
    }

    void checkStateDocumentRemoved(OperationContext* opCtx) {
        AutoGetCollection recipientColl(
            opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, MODE_IS);
        ASSERT_TRUE(bool(recipientColl));
        ASSERT_TRUE(bool(recipientColl->isEmpty(opCtx)));
    }

private:
    TypeCollectionRecipientFields _makeRecipientFields(
        const ReshardingRecipientDocument& recipientDoc) {
        TypeCollectionRecipientFields recipientFields{
            recipientDoc.getDonorShards(),
            recipientDoc.getSourceUUID(),
            recipientDoc.getSourceNss(),
            recipientDoc.getMinimumOperationDurationMillis()};

        auto donorShards = recipientFields.getDonorShards();
        for (unsigned i = 0; i < donorShards.size(); ++i) {
            auto minFetchTimestamp = Timestamp{10 + i, i};
            donorShards[i].setMinFetchTimestamp(minFetchTimestamp);
            recipientFields.setCloneTimestamp(minFetchTimestamp);
        }
        recipientFields.setDonorShards(std::move(donorShards));

        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(10000);
        approxCopySize.setApproxDocumentsToCopy(100);
        recipientFields.setReshardingApproxCopySizeStruct(std::move(approxCopySize));

        return recipientFields;
    }

    void _onReshardingFieldsChanges(OperationContext* opCtx,
                                    RecipientStateMachine& recipient,
                                    const ReshardingRecipientDocument& recipientDoc,
                                    CoordinatorStateEnum coordinatorState) {
        auto reshardingFields = TypeCollectionReshardingFields{recipientDoc.getReshardingUUID()};
        reshardingFields.setRecipientFields(_makeRecipientFields(recipientDoc));
        reshardingFields.setState(coordinatorState);
        recipient.onReshardingFieldsChanges(opCtx, reshardingFields);
    }

    std::shared_ptr<RecipientStateTransitionController> _controller;
};

TEST_F(ReshardingRecipientServiceTest, CanTransitionThroughEachStateToCompletion) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551105,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);
        auto doc = makeStateDocument(isAlsoDonor);
        auto opCtx = makeOperationContext();
        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, StepDownStepUpEachTransition) {
    const std::vector<RecipientStateEnum> recipientStates{RecipientStateEnum::kCreatingCollection,
                                                          RecipientStateEnum::kCloning,
                                                          RecipientStateEnum::kApplying,
                                                          RecipientStateEnum::kSteadyState,
                                                          RecipientStateEnum::kStrictConsistency,
                                                          RecipientStateEnum::kRenaming,
                                                          RecipientStateEnum::kDone};
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551106,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        PauseDuringStateTransitions stateTransitionsGuard{controller(), recipientStates};
        auto doc = makeStateDocument(isAlsoDonor);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());
        auto opCtx = makeOperationContext();
        auto prevState = RecipientStateEnum::kUnused;

        for (const auto state : recipientStates) {

            auto recipient = [&] {
                if (prevState == RecipientStateEnum::kUnused) {
                    if (isAlsoDonor) {
                        createSourceCollection(opCtx.get(), doc);
                    }

                    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
                    return RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());
                } else {
                    auto maybeRecipient =
                        RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
                    ASSERT_TRUE(bool(maybeRecipient));

                    // Allow the transition to prevState to succeed on this primary-only service
                    // instance.
                    stateTransitionsGuard.unset(prevState);
                    return *maybeRecipient;
                }
            }();

            if (prevState != RecipientStateEnum::kUnused) {
                // Allow the transition to prevState to succeed on this primary-only service
                // instance.
                stateTransitionsGuard.unset(prevState);
            }

            // Signal the coordinator's earliest state that allows the recipient's transition
            // into 'state' to be valid. This mimics the real system where, upon step up, the
            // new RecipientStateMachine instance gets refreshed with the coordinator's most
            // recent state.
            switch (state) {
                case RecipientStateEnum::kCreatingCollection:
                case RecipientStateEnum::kCloning: {
                    notifyToStartCloning(opCtx.get(), *recipient, doc);
                    break;
                }
                case RecipientStateEnum::kRenaming:
                case RecipientStateEnum::kDone: {
                    notifyReshardingCommitting(opCtx.get(), *recipient, doc);
                    break;
                }
                default:
                    break;
            }

            // Step down before the transition to state can complete.
            stateTransitionsGuard.wait(state);
            stepDown();

            ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                      ErrorCodes::InterruptedDueToReplStateChange);

            prevState = state;

            recipient.reset();
            stepUp(opCtx.get());
        }

        // Finally complete the operation and ensure its success.
        auto maybeRecipient = RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(bool(maybeRecipient));

        auto recipient = *maybeRecipient;

        stateTransitionsGuard.unset(RecipientStateEnum::kDone);
        notifyReshardingCommitting(opCtx.get(), *recipient, doc);
        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());
    }
}

TEST_F(ReshardingRecipientServiceTest, DropsTemporaryReshardingCollectionOnAbort) {
    for (bool isAlsoDonor : {false, true}) {
        LOGV2(5551107,
              "Running case",
              "test"_attr = _agent.getTestName(),
              "isAlsoDonor"_attr = isAlsoDonor);

        boost::optional<PauseDuringStateTransitions> doneTransitionGuard;
        doneTransitionGuard.emplace(controller(), RecipientStateEnum::kDone);

        auto doc = makeStateDocument(isAlsoDonor);
        auto instanceId =
            BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName << doc.getReshardingUUID());

        auto opCtx = makeOperationContext();

        if (isAlsoDonor) {
            // If the recipient is also a donor, the original collection should already exist on
            // this shard.
            createSourceCollection(opCtx.get(), doc);
        }

        RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
        auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

        notifyToStartCloning(opCtx.get(), *recipient, doc);
        notifyReshardingAborting(opCtx.get(), *recipient, doc);

        doneTransitionGuard->wait(RecipientStateEnum::kDone);
        stepDown();

        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(),
                  ErrorCodes::InterruptedDueToReplStateChange);

        recipient.reset();
        stepUp(opCtx.get());

        auto maybeRecipient = RecipientStateMachine::lookup(opCtx.get(), _service, instanceId);
        ASSERT_TRUE(bool(maybeRecipient));
        recipient = *maybeRecipient;

        doneTransitionGuard.reset();
        notifyReshardingAborting(opCtx.get(), *recipient, doc);

        ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
        checkStateDocumentRemoved(opCtx.get());

        if (isAlsoDonor) {
            // Verify original collection still exists after aborting.
            AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
            ASSERT_TRUE(bool(coll));
            ASSERT_EQ(coll->uuid(), doc.getSourceUUID());
        }

        // Verify the temporary collection no longer exists.
        {
            AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
            ASSERT_FALSE(bool(coll));
        }
    }
}

TEST_F(ReshardingRecipientServiceTest, RenamesTemporaryReshardingCollectionWhenDone) {
    // The temporary collection is renamed by the donor service when the shard is also a donor. Only
    // on non-donor shards will the recipient service rename the temporary collection.
    bool isAlsoDonor = false;
    boost::optional<PauseDuringStateTransitions> stateTransitionsGuard;
    stateTransitionsGuard.emplace(controller(), RecipientStateEnum::kApplying);

    auto doc = makeStateDocument(isAlsoDonor);
    auto opCtx = makeOperationContext();
    RecipientStateMachine::insertStateDocument(opCtx.get(), doc);
    auto recipient = RecipientStateMachine::getOrCreate(opCtx.get(), _service, doc.toBSON());

    notifyToStartCloning(opCtx.get(), *recipient, doc);

    // Wait to check the temporary collection has been created.
    stateTransitionsGuard->wait(RecipientStateEnum::kApplying);
    {
        // Check the temporary collection exists but is not yet renamed.
        AutoGetCollection coll(opCtx.get(), doc.getTempReshardingNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
    }
    stateTransitionsGuard.reset();

    notifyReshardingCommitting(opCtx.get(), *recipient, doc);

    ASSERT_OK(recipient->getCompletionFuture().getNoThrow());
    checkStateDocumentRemoved(opCtx.get());

    {
        // Ensure the temporary collection was renamed.
        AutoGetCollection coll(opCtx.get(), doc.getSourceNss(), MODE_IS);
        ASSERT_TRUE(bool(coll));
        ASSERT_EQ(coll->uuid(), doc.getReshardingUUID());
    }
}

}  // namespace
}  // namespace mongo

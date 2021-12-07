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

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"

namespace mongo {
namespace {

using unittest::assertGet;

class BalancerCommandsSchedulerTest : public ConfigServerTestFixture {
public:
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    const NamespaceString kNss{"testDb.testColl"};

    ChunkType makeChunk(long long min, const ShardId& shardId) {
        ChunkType chunk;
        chunk.setMin(BSON("x" << min));
        chunk.setMax(BSON("x" << min + 10));
        chunk.setJumbo(false);
        chunk.setShard(shardId);
        chunk.setVersion(ChunkVersion(1, 1, OID::gen(), Timestamp(10)));
        return chunk;
    }

    MoveChunkSettings getDefaultMoveChunkSettings() {
        return MoveChunkSettings(
            128,
            MigrationSecondaryThrottleOptions::create(
                MigrationSecondaryThrottleOptions::SecondaryThrottleOption::kDefault),
            false,
            MoveChunkRequest::ForceJumbo::kDoNotForce);
    }

    MigrationsRecoveryConfiguration getMigrationRecoveryConfiguration() {
        return MigrationsRecoveryConfiguration(
            128,
            MigrationSecondaryThrottleOptions::create(
                MigrationSecondaryThrottleOptions::SecondaryThrottleOption::kDefault));
    }

    std::vector<BSONObj> getPersistedCommandDocuments(OperationContext* opCtx) {
        auto statusWithPersistedCommandDocs =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                MigrationType::ConfigNS,
                BSONObj(),
                BSONObj(),
                boost::none);

        ASSERT_OK(statusWithPersistedCommandDocs.getStatus());
        return statusWithPersistedCommandDocs.getValue().docs;
    }


protected:
    void setUp() override {
        setUpAndInitializeConfigDb();
        setupShards(kShardList);
        // Scheduler commands target shards that need to be retrieved.
        auto opCtx = operationContext();
        configureTargeter(opCtx, kShardId0, kShardHost0);
        configureTargeter(opCtx, kShardId1, kShardHost1);
    }

    void tearDown() override {
        _scheduler.stop();
        ConfigServerTestFixture::tearDown();
    }

    void configureTargeter(OperationContext* opCtx, ShardId shardId, const HostAndPort& host) {
        auto targeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
        targeter->setFindHostReturnValue(kShardHost0);
    }

    BalancerCommandsSchedulerImpl _scheduler;
};

TEST_F(BalancerCommandsSchedulerTest, StartAndStopScheduler) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveChunkCommand) {
    auto deferredCleanupCompletedCheckpoint =
        globalFailPointRegistry().find("deferredCleanupCompletedCheckpoint");
    auto timesEnteredFailPoint =
        deferredCleanupCompletedCheckpoint->setMode(FailPoint::alwaysOn, 0);
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkType moveChunk = makeChunk(0, kShardId0);
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });
    auto futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                      kNss,
                                                      moveChunk,
                                                      ShardId(kShardId1),
                                                      getDefaultMoveChunkSettings(),
                                                      false /* issuedByRemoteUser */);
    ASSERT_OK(futureResponse.getNoThrow());
    networkResponseFuture.default_timed_get();
    deferredCleanupCompletedCheckpoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    deferredCleanupCompletedCheckpoint->setMode(FailPoint::off, 0);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMergeChunkCommand) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version(1, 1, OID::gen(), Timestamp(10));
    auto futureResponse =
        _scheduler.requestMergeChunks(operationContext(), kNss, kShardId0, range, version);
    ASSERT_OK(futureResponse.getNoThrow());
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, MergeChunkNonexistentShard) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version(1, 1, OID::gen(), Timestamp(10));
    auto futureResponse = _scheduler.requestMergeChunks(
        operationContext(), kNss, ShardId("nonexistent"), range, version);
    auto shardNotFoundError = Status{ErrorCodes::ShardNotFound, "Shard nonexistent not found"};
    ASSERT_EQ(futureResponse.getNoThrow(), shardNotFoundError);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulAutoSplitVectorCommand) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkType splitChunk = makeChunk(0, kShardId0);
    BSONObjBuilder autoSplitVectorResponse;
    autoSplitVectorResponse.append("ok", "1");
    BSONArrayBuilder splitKeys(autoSplitVectorResponse.subarrayStart("splitKeys"));
    splitKeys.append(BSON("x" << 7));
    splitKeys.append(BSON("x" << 9));
    splitKeys.done();
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return autoSplitVectorResponse.obj();
        });
    });
    auto futureResponse = _scheduler.requestAutoSplitVector(operationContext(),
                                                            kNss,
                                                            splitChunk.getShard(),
                                                            BSON("x" << 1),
                                                            splitChunk.getMin(),
                                                            splitChunk.getMax(),
                                                            4);
    auto swReceivedSplitKeys = futureResponse.getNoThrow();
    ASSERT_OK(swReceivedSplitKeys.getStatus());
    auto receivedSplitKeys = swReceivedSplitKeys.getValue();
    ASSERT_EQ(receivedSplitKeys.size(), 2);
    ASSERT_BSONOBJ_EQ(receivedSplitKeys[0], BSON("x" << 7));
    ASSERT_BSONOBJ_EQ(receivedSplitKeys[1], BSON("x" << 9));
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulSplitChunkCommand) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkType splitChunk = makeChunk(0, kShardId0);
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });
    auto futureResponse = _scheduler.requestSplitChunk(operationContext(),
                                                       kNss,
                                                       splitChunk.getShard(),
                                                       splitChunk.getVersion(),
                                                       KeyPattern(BSON("x" << 1)),
                                                       splitChunk.getMin(),
                                                       splitChunk.getMax(),
                                                       std::vector<BSONObj>{BSON("x" << 5)});
    ASSERT_OK(futureResponse.getNoThrow());
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulRequestChunkDataSizeCommand) {
    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkType chunk = makeChunk(0, kShardId0);
    BSONObjBuilder chunkSizeResponse;
    chunkSizeResponse.append("ok", "1");
    chunkSizeResponse.append("size", 156);
    chunkSizeResponse.append("numObjects", 25);
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return chunkSizeResponse.obj(); });
    });
    auto futureResponse = _scheduler.requestDataSize(operationContext(),
                                                     kNss,
                                                     chunk.getShard(),
                                                     chunk.getRange(),
                                                     chunk.getVersion(),
                                                     KeyPattern(BSON("x" << 1)),
                                                     false /* issuedByRemoteUser */);
    auto swReceivedDataSize = futureResponse.getNoThrow();
    ASSERT_OK(swReceivedDataSize.getStatus());
    auto receivedDataSize = swReceivedDataSize.getValue();
    ASSERT_EQ(receivedDataSize.sizeBytes, 156);
    ASSERT_EQ(receivedDataSize.numObjects, 25);
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenNetworkReturnsError) {
    auto deferredCleanupCompletedCheckpoint =
        globalFailPointRegistry().find("deferredCleanupCompletedCheckpoint");
    auto timesEnteredFailPoint =
        deferredCleanupCompletedCheckpoint->setMode(FailPoint::alwaysOn, 0);

    _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
    ChunkType moveChunk = makeChunk(0, kShardId0);
    auto timeoutError = Status{ErrorCodes::NetworkTimeout, "Mock error: network timed out"};
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&](const executor::RemoteCommandRequest& request) { return timeoutError; });
    });
    auto futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                      kNss,
                                                      moveChunk,
                                                      ShardId(kShardId1),
                                                      getDefaultMoveChunkSettings(),
                                                      false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(), timeoutError);
    networkResponseFuture.default_timed_get();
    deferredCleanupCompletedCheckpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    deferredCleanupCompletedCheckpoint->setMode(FailPoint::off, 0);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenSchedulerIsStopped) {
    ChunkType moveChunk = makeChunk(0, kShardId0);
    auto futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                      kNss,
                                                      moveChunk,
                                                      ShardId(kShardId1),
                                                      getDefaultMoveChunkSettings(),
                                                      false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request rejected - balancer scheduler is stopped"));
    // Ensure DistLock is not taken
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
}

TEST_F(BalancerCommandsSchedulerTest, CommandCanceledIfBalancerStops) {
    SemiFuture<void> futureResponse;
    {
        FailPointEnableBlock failPoint("pauseSubmissionsFailPoint");
        _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
        ChunkType moveChunk = makeChunk(0, kShardId0);
        futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                     kNss,
                                                     moveChunk,
                                                     ShardId(kShardId1),
                                                     getDefaultMoveChunkSettings(),
                                                     false /* issuedByRemoteUser */);
        _scheduler.stop();
    }
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request cancelled - balancer scheduler is stopping"));
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
}

TEST_F(BalancerCommandsSchedulerTest, MoveChunkCommandGetsPersistedOnDiskWhenRequestIsSubmitted) {
    auto opCtx = operationContext();
    _scheduler.start(opCtx, getMigrationRecoveryConfiguration());
    ChunkType moveChunk = makeChunk(0, kShardId0);
    auto requestSettings = getDefaultMoveChunkSettings();
    auto const serviceContext = getServiceContext();
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&, serviceContext](const executor::RemoteCommandRequest& request) {
            ThreadClient tc("Test", getGlobalServiceContext());
            auto opCtxHolder = Client::getCurrent()->makeOperationContext();
            // As long as the request is not completed, a persisted recovery document should
            // exist...
            auto persistedCommandDocs = getPersistedCommandDocuments(opCtxHolder.get());
            ASSERT_EQUALS(1, persistedCommandDocs.size());
            auto swPersistedCommand = MigrationType::fromBSON(persistedCommandDocs[0]);

            // ... with the needed info to reconstruct an equivalent command.
            ASSERT_OK(swPersistedCommand.getStatus());
            auto recoveredCommand = MoveChunkCommandInfo::recoverFrom(
                swPersistedCommand.getValue(),
                MigrationsRecoveryConfiguration(requestSettings.maxChunkSizeBytes,
                                                requestSettings.secondaryThrottle));
            ASSERT_EQ(kNss, recoveredCommand->getNameSpace());
            ASSERT_EQ(moveChunk.getShard(), recoveredCommand->getTarget());
            ASSERT_TRUE(recoveredCommand->requiresDistributedLock());
            MoveChunkCommandInfo originalCommandInfo(kNss,
                                                     moveChunk.getShard(),
                                                     kShardId1,
                                                     moveChunk.getMin(),
                                                     moveChunk.getMax(),
                                                     requestSettings.maxChunkSizeBytes,
                                                     requestSettings.secondaryThrottle,
                                                     requestSettings.waitForDelete,
                                                     requestSettings.forceJumbo,
                                                     moveChunk.getVersion(),
                                                     boost::none);
            ASSERT_BSONOBJ_EQ(originalCommandInfo.serialise(), recoveredCommand->serialise());

            return BSON("ok" << true);
        });
    });


    auto deferredResponse = _scheduler.requestMoveChunk(operationContext(),
                                                        kNss,
                                                        moveChunk,
                                                        ShardId(kShardId1),
                                                        requestSettings,
                                                        false /* issuedByRemoteUser */);
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, PersistedCommandsAreReissuedWhenRecoveringFromCrash) {
    auto opCtx = operationContext();
    // 1. Insert a recovery document on an outstanding migration.
    ChunkType moveChunk = makeChunk(0, kShardId0);
    auto requestSettings = getDefaultMoveChunkSettings();
    MigrationType recoveryInfo(kNss,
                               moveChunk.getMin(),
                               moveChunk.getMax(),
                               moveChunk.getShard(),
                               kShardId1,
                               moveChunk.getVersion(),
                               requestSettings.waitForDelete,
                               requestSettings.forceJumbo);
    ASSERT_OK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx,
        MigrationType::ConfigNS,
        recoveryInfo.toBSON(),
        ShardingCatalogClient::kMajorityWriteConcern));

    // 2. Once started, the persisted document should trigger the remote execution of a request...
    _scheduler.start(opCtx, getMigrationRecoveryConfiguration());
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            auto expectedCommandInfo = MoveChunkCommandInfo::recoverFrom(
                recoveryInfo, getMigrationRecoveryConfiguration());
            // 3. ... Which content should match the recovery doc & configuration.
            ASSERT_BSONOBJ_EQ(expectedCommandInfo->serialise(), request.cmdObj);
            return BSON("ok" << true);
        });
    });

    // 4. Once the recovery phase is complete, no persisted documents should remain
    //    (stop() is invoked to ensure that the observed state is stable).
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
    auto persistedCommandDocs = getPersistedCommandDocuments(operationContext());
    ASSERT_EQUALS(0, persistedCommandDocs.size());
}

TEST_F(BalancerCommandsSchedulerTest, DistLockPreventsMoveChunkWithConcurrentDDL) {
    OperationContext* opCtx;
    FailPoint* failpoint = globalFailPointRegistry().find("pauseSubmissionsFailPoint");
    failpoint->setMode(FailPoint::Mode::alwaysOn);
    {
        _scheduler.start(operationContext(), getMigrationRecoveryConfiguration());
        opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
        failpoint->setMode(FailPoint::Mode::off);
        ChunkType moveChunk = makeChunk(0, kShardId0);
        auto futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                          kNss,
                                                          moveChunk,
                                                          ShardId(kShardId1),
                                                          getDefaultMoveChunkSettings(),
                                                          false /* issuedByRemoteUser */);
        ASSERT_EQ(
            futureResponse.getNoThrow(),
            Status(ErrorCodes::LockBusy, "Failed to acquire dist lock testDb.testColl locally"));
    }
    _scheduler.stop();
}

}  // namespace
}  // namespace mongo

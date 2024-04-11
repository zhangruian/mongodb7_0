/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/vector_clock.h"
#include "mongo/platform/basic.h"
#include "mongo/s/client/config_shard_wrapper.h"
#include "mongo/s/sharding_router_test_fixture.h"

namespace mongo {
namespace {

class MockShard : public Shard {
    MockShard(const MockShard&) = delete;
    MockShard& operator=(const MockShard&) = delete;

public:
    explicit MockShard(const ShardId& id) : Shard(id) {}
    ~MockShard() = default;

    ConnectionString getConnString() const override {
        const HostAndPort configHost{"configHost1"};
        const ConnectionString configCS{
            ConnectionString::forReplicaSet("configReplSet", {configHost})};
        return configCS;
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() const override {
        return std::make_shared<RemoteCommandTargeterMock>();
    }

    void updateReplSetMonitor(const HostAndPort& remoteHost,
                              const Status& remoteCommandStatus) override {}

    std::string toString() const override {
        return getId().toString();
    }

    bool isRetriableError(ErrorCodes::Error code, RetryPolicy options) final {
        return false;
    }

    void runFireAndForgetCommand(OperationContext* opCtx,
                                 const ReadPreferenceSetting& readPref,
                                 const std::string& dbName,
                                 const BSONObj& cmdObj) override {
        lastReadPref = readPref;
    }
    Status runAggregation(
        OperationContext* opCtx,
        const AggregateCommandRequest& aggRequest,
        std::function<bool(const std::vector<BSONObj>& batch,
                           const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
        return Status::OK();
    }

    BatchedCommandResponse runBatchWriteCommand(OperationContext* opCtx,
                                                Milliseconds maxTimeMS,
                                                const BatchedCommandRequest& batchRequest,
                                                const WriteConcernOptions& writeConcern,
                                                RetryPolicy retryPolicy) override {
        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        return response;
    }


    ReadPreferenceSetting lastReadPref;

private:
    StatusWith<Shard::CommandResponse> _runCommand(OperationContext* opCtx,
                                                   const ReadPreferenceSetting& readPref,
                                                   StringData dbName,
                                                   Milliseconds maxTimeMSOverride,
                                                   const BSONObj& cmdObj) final {
        lastReadPref = readPref;
        return Shard::CommandResponse{boost::none, BSON("ok" << 1), Status::OK(), Status::OK()};
    }

    StatusWith<Shard::QueryResponse> _runExhaustiveCursorCommand(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        StringData dbName,
        Milliseconds maxTimeMSOverride,
        const BSONObj& cmdObj) final {
        lastReadPref = readPref;
        return Shard::QueryResponse{std::vector<BSONObj>{}, repl::OpTime::max()};
    }

    StatusWith<Shard::QueryResponse> _exhaustiveFindOnConfig(
        OperationContext* opCtx,
        const ReadPreferenceSetting& readPref,
        const repl::ReadConcernLevel& readConcernLevel,
        const NamespaceString& nss,
        const BSONObj& query,
        const BSONObj& sort,
        boost::optional<long long> limit,
        const boost::optional<BSONObj>& hint = boost::none) final {
        lastReadPref = readPref;
        return Shard::QueryResponse{std::vector<BSONObj>{}, repl::OpTime::max()};
    }
};

class ConfigShardWrapperTest : public ShardingTestFixture {
protected:
    std::shared_ptr<MockShard> _mockConfigShard;
    std::unique_ptr<ConfigShardWrapper> _configShardWrapper;

    void setUp() override {
        serverGlobalParams.clusterRole = ClusterRole::ConfigServer;

        ShardingTestFixture::setUp();

        _mockConfigShard = std::make_shared<MockShard>(ShardId(ShardId::kConfigServerId));
        _configShardWrapper = std::make_unique<ConfigShardWrapper>(_mockConfigShard);
    }

    void tearDown() override {
        ShardingTestFixture::tearDown();
    }
};

TEST_F(ConfigShardWrapperTest, RunCommandAttachesMinClusterTime) {
    const auto vcTime = VectorClock::get(operationContext())->getTime();
    auto expectedMinClusterTime = vcTime.configTime();
    expectedMinClusterTime.addTicks(10);
    VectorClock::get(operationContext())->advanceConfigTime_forTest(expectedMinClusterTime);

    auto result = _configShardWrapper->runCommand(operationContext(),
                                                  ReadPreferenceSetting{},
                                                  DatabaseName::kConfig.db().toString(),
                                                  BSONObj{},
                                                  Shard::RetryPolicy::kNoRetry);

    ASSERT_EQ(_mockConfigShard->lastReadPref.minClusterTime, expectedMinClusterTime.asTimestamp());
}

TEST_F(ConfigShardWrapperTest, RunFireAndForgetCommandAttachesMinClusterTime) {
    const auto vcTime = VectorClock::get(operationContext())->getTime();
    auto expectedMinClusterTime = vcTime.configTime();
    expectedMinClusterTime.addTicks(10);
    VectorClock::get(operationContext())->advanceConfigTime_forTest(expectedMinClusterTime);

    _configShardWrapper->runFireAndForgetCommand(operationContext(),
                                                 ReadPreferenceSetting{},
                                                 DatabaseName::kConfig.db().toString(),
                                                 BSONObj{});

    ASSERT_EQ(_mockConfigShard->lastReadPref.minClusterTime, expectedMinClusterTime.asTimestamp());
}

TEST_F(ConfigShardWrapperTest, GetConfigShardReturnsConfigShardWrapper) {
    ASSERT_EQ(typeid(*shardRegistry()->getConfigShard()).name(), typeid(ConfigShardWrapper).name());
}

}  // namespace
}  // namespace mongo

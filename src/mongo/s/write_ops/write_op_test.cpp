/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/concurrency/locker_mongos_client_observer.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("foo.bar");

write_ops::DeleteOpEntry buildDelete(const BSONObj& query, bool multi) {
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(multi);
    return entry;
}

void sortByEndpoint(std::vector<std::unique_ptr<TargetedWrite>>* writes) {
    struct EndpointComp {
        bool operator()(const std::unique_ptr<TargetedWrite>& writeA,
                        const std::unique_ptr<TargetedWrite>& writeB) const {
            return writeA->endpoint.shardName.compare(writeB->endpoint.shardName) < 0;
        }
    };
    std::sort(writes->begin(), writes->end(), EndpointComp());
}

class WriteOpTest : public ServiceContextTest {
protected:
    static Status getMockRetriableError(CollectionGeneration& gen) {
        return {StaleConfigInfo(
                    kNss,
                    ShardVersionFactory::make(ChunkVersion(gen, {10, 0}),
                                              boost::optional<CollectionIndexes>(boost::none)),
                    ShardVersionFactory::make(ChunkVersion(gen, {11, 0}),
                                              boost::optional<CollectionIndexes>(boost::none)),
                    ShardId("shardA")),
                "simulate ssv error for test"};
    }

    static Status getMockNonRetriableError(CollectionGeneration& gen) {
        return {DuplicateKeyErrorInfo(
                    BSON("mock" << 1), BSON("" << 1), BSONObj{}, stdx::monostate{}, boost::none),
                "Mock duplicate key error"};
    }

    WriteOp setupTwoShardTest(CollectionGeneration& gen,
                              std::vector<std::unique_ptr<TargetedWrite>>& targeted,
                              bool isTransactional) const {
        ShardEndpoint endpointA(
            ShardId("shardA"),
            ShardVersionFactory::make(ChunkVersion(gen, {10, 0}),
                                      boost::optional<CollectionIndexes>(boost::none)),
            boost::none);
        ShardEndpoint endpointB(
            ShardId("shardB"),
            ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                      boost::optional<CollectionIndexes>(boost::none)),
            boost::none);

        BatchedCommandRequest request([&] {
            write_ops::DeleteCommandRequest deleteOp(kNss);
            deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
            return deleteOp;
        }());

        if (isTransactional) {
            const TxnNumber kTxnNumber = 1;
            _opCtx->setTxnNumber(kTxnNumber);

            auto txnRouter = TransactionRouter::get(_opCtx);
            txnRouter.beginOrContinueTxn(
                _opCtx, kTxnNumber, TransactionRouter::TransactionActions::kStart);
        }

        // Do multi-target write op
        WriteOp writeOp(BatchItemRef(&request, 0), isTransactional);
        ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

        MockNSTargeter targeter(kNss,
                                {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                                 MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))});

        writeOp.targetWrites(_opCtx, targeter, &targeted);
        ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
        ASSERT_EQUALS(targeted.size(), 2u);
        sortByEndpoint(&targeted);
        ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
        if (!isTransactional) {
            ASSERT(ShardVersion::isIgnoredVersion(*targeted[0]->endpoint.shardVersion));
        }
        ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);
        if (!isTransactional) {
            ASSERT(ShardVersion::isIgnoredVersion(*targeted[1]->endpoint.shardVersion));
        }

        return writeOp;
    }

    WriteOpTest() {
        auto service = getServiceContext();
        service->registerClientObserver(std::make_unique<LockerMongosClientObserver>());
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
};

// Test of basic error-setting on write op
TEST_F(WriteOpTest, BasicError) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    write_ops::WriteError error(0, {ErrorCodes::UnknownError, "some message"});
    writeOp.setOpError(error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), error.getStatus());
}

TEST_F(WriteOpTest, TargetSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.noteWriteComplete(*targeted.front());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our query goes to one shard
TEST_F(WriteOpTest, TargetMultiOneShard) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    ShardEndpoint endpointA(
        ShardId("shardA"),
        ShardVersionFactory::make(ChunkVersion(gen, {10, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointC(
        ShardId("shardB"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        // Only hits first shard
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -2 << LT << -1), false)});
        return deleteOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpointA);

    writeOp.noteWriteComplete(*targeted.front());

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our write goes to more than one shard
TEST_F(WriteOpTest, TargetMultiAllShards) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    ShardEndpoint endpointA(
        ShardId("shardA"),
        ShardVersionFactory::make(ChunkVersion(gen, {10, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointC(
        ShardId("shardB"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
        return deleteOp;
    }());

    // Do multi-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 3u);
    sortByEndpoint(&targeted);
    ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
    ASSERT(ShardVersion::isIgnoredVersion(*targeted[0]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);
    ASSERT(ShardVersion::isIgnoredVersion(*targeted[1]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[2]->endpoint.shardName, endpointC.shardName);
    ASSERT(ShardVersion::isIgnoredVersion(*targeted[2]->endpoint.shardVersion));

    writeOp.noteWriteComplete(*targeted[0]);
    writeOp.noteWriteComplete(*targeted[1]);
    writeOp.noteWriteComplete(*targeted[2]);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

TEST_F(WriteOpTest, TargetMultiAllShardsAndErrorSingleChildOp1) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    auto writeOp = setupTwoShardTest(gen, targeted, false);

    // Simulate retryable error.
    write_ops::WriteError retryableError(0, getMockRetriableError(gen));
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State should not change until we have result from all nodes.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    writeOp.noteWriteComplete(*targeted[1]);

    // State resets back to ready because of retryable error.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

TEST_F(WriteOpTest, TargetMultiAllShardsAndErrorMultipleChildOp2) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    auto writeOp = setupTwoShardTest(gen, targeted, false);

    // Simulate two errors: one retryable error and another non-retryable error.
    write_ops::WriteError retryableError(0, getMockRetriableError(gen));
    write_ops::WriteError nonRetryableError(1, getMockNonRetriableError(gen));

    // First, the retryable error is issued.
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State should not change until we have result from all nodes.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    // Then, the non-retyrable error is issued.
    writeOp.noteWriteError(*targeted[1], nonRetryableError);

    // State remains in error, because of non-retryable error.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), nonRetryableError.getStatus());
}

TEST_F(WriteOpTest, TargetMultiAllShardsAndErrorMultipleChildOp3) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    auto writeOp = setupTwoShardTest(gen, targeted, false);

    // Simulate two errors: one non-retryable error and another retryable error.
    write_ops::WriteError retryableError(0, getMockRetriableError(gen));
    write_ops::WriteError nonRetryableError(1, getMockNonRetriableError(gen));

    // First, the non-retryable error is issued.
    writeOp.noteWriteError(*targeted[1], nonRetryableError);

    // State should not change until we have result from all nodes.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    // Then, the retyrable error is issued.
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State remains in error, because of non-retryable error.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), nonRetryableError.getStatus());
}

// Single error after targeting test
TEST_F(WriteOpTest, ErrorSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    write_ops::WriteError error(0, {ErrorCodes::UnknownError, "some message"});
    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), error.getStatus());
}

// Cancel single targeting test
TEST_F(WriteOpTest, CancelSingle) {
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.cancelWrites(nullptr);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

//
// Test retryable errors
//

// Retry single targeting test
TEST_F(WriteOpTest, RetrySingleOp) {
    ShardEndpoint endpoint(ShardId("shard"), ShardVersion::IGNORED(), boost::none);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(kNss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    // Stale exception
    write_ops::WriteError error(
        0,
        {StaleConfigInfo(kNss, ShardVersion::IGNORED(), boost::none, ShardId("shard")),
         "some message"});
    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

class WriteOpTransactionTest : public WriteOpTest {
private:
    RouterOperationContextSession _routerOpCtxSession{[this] {
        _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        return _opCtx;
    }()};
};

TEST_F(WriteOpTransactionTest, TargetMultiDoesNotTargetAllShards) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    ShardEndpoint endpointA(
        ShardId("shardA"),
        ShardVersionFactory::make(ChunkVersion(gen, {10, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointB(
        ShardId("shardB"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointC(
        ShardId("shardC"),
        ShardVersionFactory::make(ChunkVersion(gen, {20, 0}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), true /*multi*/)});
        return deleteOp;
    }());

    // Target the multi-write.
    WriteOp writeOp(BatchItemRef(&request, 0), false);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter(kNss,
                            {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                             MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                             MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    writeOp.targetWrites(_opCtx, targeter, &targeted);

    // The write should only target shardA and shardB and send real shard versions to each.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 2u);
    sortByEndpoint(&targeted);
    assertEndpointsEqual(targeted.front()->endpoint, endpointA);
    assertEndpointsEqual(targeted.back()->endpoint, endpointB);

    writeOp.noteWriteComplete(*targeted[0]);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);

    writeOp.noteWriteComplete(*targeted[1]);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

TEST_F(WriteOpTransactionTest, TargetMultiAllShardsAndErrorSingleChildOp1) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    auto writeOp = setupTwoShardTest(gen, targeted, true);

    // Simulate retryable error.
    write_ops::WriteError retryableError(0, getMockRetriableError(gen));
    writeOp.noteWriteError(*targeted[0], retryableError);

    // State should change to error right away even with retryable error when in a transaction.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), retryableError.getStatus());
}

TEST_F(WriteOpTransactionTest, TargetMultiAllShardsAndErrorSingleChildOp2) {
    CollectionGeneration gen(OID(), Timestamp(1, 1));
    std::vector<std::unique_ptr<TargetedWrite>> targeted;
    auto writeOp = setupTwoShardTest(gen, targeted, true);

    // Simulate non-retryable error.
    write_ops::WriteError nonRetryableError(0, getMockRetriableError(gen));
    writeOp.noteWriteError(*targeted[0], nonRetryableError);

    // State should change to error right away even with non-retryable error when in a transaction.
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getStatus(), nonRetryableError.getStatus());
}

}  // namespace
}  // namespace mongo

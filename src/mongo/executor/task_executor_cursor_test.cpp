/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/executor/pinned_connection_task_executor.h"
#include "mongo/executor/pinned_connection_task_executor_test_fixture.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {

BSONObj buildCursorResponse(StringData fieldName, size_t start, size_t end, size_t cursorId) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursor(bob.subobjStart("cursor"));
        {
            BSONArrayBuilder batch(cursor.subarrayStart(fieldName));

            for (size_t i = start; i <= end; ++i) {
                BSONObjBuilder doc(batch.subobjStart());
                doc.append("x", int(i));
            }
        }
        cursor.append("id", (long long)(cursorId));
        cursor.append("ns", "test.test");
    }
    bob.append("ok", int(1));
    return bob.obj();
}

BSONObj buildMultiCursorResponse(StringData fieldName,
                                 size_t start,
                                 size_t end,
                                 std::vector<size_t> cursorIds) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder cursors;
        int baseCursorValue = 1;
        for (auto cursorId : cursorIds) {
            BSONObjBuilder cursor;
            BSONArrayBuilder batch;
            ASSERT(start < end && end < INT_MAX);
            for (size_t i = start; i <= end; ++i) {
                batch.append(BSON("x" << static_cast<int>(i) * baseCursorValue).getOwned());
            }
            cursor.append(fieldName, batch.arr());
            cursor.append("id", (long long)(cursorId));
            cursor.append("ns", "test.test");
            auto cursorObj = BSON("cursor" << cursor.done() << "ok" << 1);
            cursors.append(cursorObj.getOwned());
            ++baseCursorValue;
        }
        bob.append("cursors", cursors.arr());
    }
    bob.append("ok", 1);
    return bob.obj();
}

/**
 * Fixture for the task executor cursor tests which offers some convenience methods to help with
 * scheduling responses. Uses the CRTP pattern so that the tests can be shared between child-classes
 * that provide their own implementations of the network-mocking needed for the tests.
 */
template <typename Derived, typename Base>
class TaskExecutorCursorTestFixture : public Base {
public:
    void setUp() override {
        Base::setUp();
        client = serviceCtx->makeClient("TaskExecutorCursorTest");
        opCtx = client->makeOperationContext();
        static_cast<Derived*>(this)->postSetUp();
    }

    void tearDown() override {
        opCtx.reset();
        client.reset();

        Base::tearDown();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        return static_cast<Derived*>(this)->scheduleSuccessfulCursorResponse(
            fieldName, start, end, cursorId);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        return static_cast<Derived*>(this)->scheduleSuccessfulMultiCursorResponse(
            fieldName, start, end, cursorIds);
    }

    void scheduleErrorResponse(Status error) {
        return static_cast<Derived*>(this)->scheduleErrorResponse(error);
    }
    void blackHoleNextOutgoingRequest() {
        return static_cast<Derived*>(this)->blackHoleNextOutgoingRequest();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {
        return static_cast<Derived*>(this)->scheduleSuccessfulKillCursorResponse(cursorId);
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        return static_cast<Derived*>(this)->makeTec(rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return static_cast<Derived*>(this)->hasReadyRequests();
    }

    Base& asBase() {
        return *this;
    }

    /**
     * Ensure we work for a single simple batch
     */
    void SingleBatchWorksTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const CursorId cursorId = 0;

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_FALSE(hasReadyRequests());

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));
    }

    /**
     * Ensure the firstBatch can be read correctly when multiple cursors are returned.
     */
    void MultipleCursorsSingleBatchSucceedsTest() {
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", aggCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }
    /**
     * The operation context under which we send the original cursor-establishing command
     * can be destructed before getNext is called with new opCtx. Ensure that 'child'
     * TaskExecutorCursors created from the original TEC's multi-cursor-response can safely
     * operate if this happens/don't try and use the now-destroyed operation context.
     * See SERVER-69702 for context
     */
    void ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest() {
        auto lsid = makeLogicalSessionIdForTest();
        opCtx->setLogicalSessionId(lsid);
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));
        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", aggCmd, opCtx.get());
        TaskExecutorCursor tec = makeTec(rcr);
        auto expected = BSON("aggregate"
                             << "test"
                             << "pipeline" << BSON_ARRAY(BSON("returnMultipleCursors" << true))
                             << "lsid" << lsid.toBSON());
        ASSERT_BSONOBJ_EQ(expected,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, {0, 0}));
        // Before calling getNext (and therefore spawning child TECs), destroy the opCtx
        // we used to send the initial query and make a new one.
        opCtx.reset();
        opCtx = client->makeOperationContext();
        opCtx->setLogicalSessionId(lsid);
        // Use the new opCtx to call getNext. The child TECs should not attempt to read from the
        // now dead original opCtx.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        ASSERT_FALSE(tec.getNext(opCtx.get()));

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }

    void MultipleCursorsGetMoreWorksTest() {
        const auto aggCmd = BSON("aggregate"
                                 << "test"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("returnMultipleCursors" << true)));

        std::vector<size_t> cursorIds{1, 2};
        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", aggCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        ASSERT_BSONOBJ_EQ(aggCmd,
                          scheduleSuccessfulMultiCursorResponse("firstBatch", 1, 2, cursorIds));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        auto cursorVec = tec.releaseAdditionalCursors();
        ASSERT_EQUALS(cursorVec.size(), 1);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorIds[0]));

        // Repeat for second cursor.
        auto secondCursor = std::move(cursorVec[0]);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 2);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 4);

        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { secondCursor.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        ASSERT_BSONOBJ_EQ(BSON("getMore" << 2LL << "collection"
                                         << "test"),
                          scheduleSuccessfulCursorResponse("nextBatch", 6, 8, cursorIds[1]));
        // Read second batch, then schedule EOF on both cursors.
        // Then read final document for each.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 5);
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, 0);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 6);

        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 6);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 7);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 8);
        scheduleSuccessfulCursorResponse("nextBatch", 12, 12, 0);
        ASSERT_EQUALS(secondCursor.getNext(opCtx.get()).value()["x"].Int(), 12);

        // Shouldn't have any more requests, both cursors are closed.
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec.getNext(opCtx.get()));
        ASSERT_FALSE(secondCursor.getNext(opCtx.get()));
    }

    /**
     * Ensure we work if find fails (and that we receive the error code it failed with)
     */
    void FailureInFindTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr);

        scheduleErrorResponse(Status(ErrorCodes::BadValue, "an error"));

        ASSERT_THROWS_CODE(tec.getNext(opCtx.get()), DBException, ErrorCodes::BadValue);
    }


    /**
     * Ensure multiple batches works correctly
     */
    void MultipleBatchesWorksTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        // ASSERT(hasReadyRequests());

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        // If we try to getNext() at this point, we are interruptible and can timeout
        ASSERT_THROWS_CODE(opCtx->runWithDeadline(Date_t::now() + Milliseconds(100),
                                                  ErrorCodes::ExceededTimeLimit,
                                                  [&] { tec.getNext(opCtx.get()); }),
                           DBException,
                           ErrorCodes::ExceededTimeLimit);

        // We can pick up after that interruption though
        ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                         << "test"
                                         << "batchSize" << 3),
                          scheduleSuccessfulCursorResponse("nextBatch", 3, 5, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 3);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 4);
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 5);

        cursorId = 0;
        scheduleSuccessfulCursorResponse("nextBatch", 6, 6, cursorId);

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 6);

        // We don't issue extra getmores after returning a 0 cursor id
        ASSERT_FALSE(hasReadyRequests());

        ASSERT_FALSE(tec.getNext(opCtx.get()));
    }

    /**
     * Ensure we allow empty firstBatch.
     */
    void EmptyFirstBatchTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        // Schedule a cursor response with an empty "firstBatch". Use end < start so we don't
        // append any doc to "firstBatch".
        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 0, cursorId));

        stdx::thread th([&] {
            // Wait for the getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd, scheduleSuccessfulCursorResponse("nextBatch", 1, 1, 0));
        });

        // Verify that the first doc is the doc from the second batch.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        th.join();
    }

    /**
     * Ensure we allow any empty non-initial batch.
     */
    void EmptyNonInitialBatchTest() {
        const auto findCmd = BSON("find"
                                  << "test"
                                  << "batchSize" << 2);
        const auto getMoreCmd = BSON("getMore" << 1LL << "collection"
                                               << "test"
                                               << "batchSize" << 3);
        const CursorId cursorId = 1;

        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

        TaskExecutorCursor tec = makeTec(rcr, [] {
            TaskExecutorCursor::Options opts;
            opts.batchSize = 3;
            return opts;
        }());

        // Schedule a cursor response with a non-empty "firstBatch".
        ASSERT_BSONOBJ_EQ(findCmd, scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 1);

        // Schedule two consecutive cursor responses with empty "nextBatch". Use end < start so
        // we don't append any doc to "nextBatch".
        ASSERT_BSONOBJ_EQ(getMoreCmd,
                          scheduleSuccessfulCursorResponse("nextBatch", 1, 0, cursorId));

        stdx::thread th([&] {
            // Wait for the first getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd,
                              scheduleSuccessfulCursorResponse("nextBatch", 1, 0, cursorId));

            // Wait for the second getMore run by the getNext() below to be ready, and schedule a
            // cursor response with a non-empty "nextBatch".
            while (!hasReadyRequests()) {
                sleepmillis(10);
            }

            ASSERT_BSONOBJ_EQ(getMoreCmd, scheduleSuccessfulCursorResponse("nextBatch", 2, 2, 0));
        });

        // Verify that the next doc is the doc from the fourth batch.
        ASSERT_EQUALS(tec.getNext(opCtx.get()).value()["x"].Int(), 2);

        th.join();
    }

    ServiceContext::UniqueServiceContext serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

class NonPinningTaskExecutorCursorTestFixture
    : public TaskExecutorCursorTestFixture<NonPinningTaskExecutorCursorTestFixture,
                                           ThreadPoolExecutorTest> {
public:
    void postSetUp() {
        launchExecutorThread();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());


        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildCursorResponse(fieldName, start, end, cursorId));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());


        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildMultiCursorResponse(fieldName, start, end, cursorIds));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        auto rcr = getNet()->scheduleSuccessfulResponse(
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    void scheduleErrorResponse(Status error) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleErrorResponse(error);
        getNet()->runReadyNetworkOperations();
    }

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        return getNet()->hasReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->blackHole(getNet()->getFrontOfUnscheduledQueue());
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }
};

class PinnedConnTaskExecutorCursorTestFixture
    : public TaskExecutorCursorTestFixture<PinnedConnTaskExecutorCursorTestFixture,
                                           PinnedConnectionTaskExecutorTest> {
public:
    void postSetUp() {}

    BSONObj scheduleResponse(StatusWith<BSONObj> response) {
        int32_t responseToId;
        BSONObj cmdObjReceived;
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            responseToId = m.header().getId();
            auto opMsg = OpMsgRequest::parse(m);
            cmdObjReceived = opMsg.body.removeField("$db").getOwned();
            pf.promise.emplaceValue();
            return Status::OK();
        });
        // Wait until we recieved the command request.
        pf.future.get();

        // Now we expect source message to be called and provide the response
        expectSourceMessage([=]() {
            rpc::OpMsgReplyBuilder replyBuilder;
            replyBuilder.setCommandReply(response);
            auto message = replyBuilder.done();
            message.header().setResponseToMsgId(responseToId);
            return message;
        });
        return cmdObjReceived;
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId) {
        auto cursorResponse = buildCursorResponse(fieldName, start, end, cursorId);
        return scheduleResponse(cursorResponse);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds) {
        auto cursorResponse = buildMultiCursorResponse(fieldName, start, end, cursorIds);
        return scheduleResponse(cursorResponse);
    }

    void scheduleErrorResponse(Status error) {
        scheduleResponse(error);
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId) {

        auto cursorResponse =
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1);
        return scheduleResponse(cursorResponse);
    }

    TaskExecutorCursor makeTec(RemoteCommandRequest rcr,
                               TaskExecutorCursor::Options&& options = {}) {
        options.pinConnection = true;
        return TaskExecutorCursor(getExecutorPtr(), rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return asBase().hasReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            pf.promise.emplaceValue();
            return Status(ErrorCodes::SocketException, "test");
        });
        pf.future.get();
    }
};

TEST_F(NonPinningTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, SingleBatchWorks) {
    SingleBatchWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleCursorsSingleBatchSucceeds) {
    MultipleCursorsSingleBatchSucceedsTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture,
       ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructed) {
    ChildTaskExecutorCursorsAreSafeIfOriginalOpCtxDestructedTest();
}
TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleCursorsGetMoreWorks) {
    MultipleCursorsGetMoreWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, FailureInFind) {
    FailureInFindTest();
}

/**
 * Ensure early termination of the cursor calls killCursor (if we know about the cursor id)
 * Only applicapble to the unpinned case - if the connection is pinned, and a getMore is
 * in progress and/or fails, the most we can do is kill the connection. We can't re-use
 * the connection to send killCursors.
 */
TEST_F(NonPinningTaskExecutorCursorTestFixture, EarlyReturnKillsCursor) {
    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 2);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    {
        TaskExecutorCursor tec = makeTec(rcr);

        scheduleSuccessfulCursorResponse("firstBatch", 1, 2, cursorId);

        ASSERT(tec.getNext(opCtx.get()));

        // Black hole the pending `getMore` operation scheduled by the `TaskExecutorCursor`.
        blackHoleNextOutgoingRequest();
    }


    ASSERT_BSONOBJ_EQ(BSON("killCursors"
                           << "test"
                           << "cursors" << BSON_ARRAY(1)),
                      scheduleSuccessfulKillCursorResponse(1));
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, MultipleBatchesWorks) {
    MultipleBatchesWorksTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, EmptyFirstBatch) {
    EmptyFirstBatchTest();
}

TEST_F(NonPinningTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

TEST_F(PinnedConnTaskExecutorCursorTestFixture, EmptyNonInitialBatch) {
    EmptyNonInitialBatchTest();
}

/**
 * Ensure the LSID is passed in all stages of querying. Need to test the
 * pinning case separately because of difference around killCursor.
 */
TEST_F(NonPinningTaskExecutorCursorTestFixture, LsidIsPassed) {
    auto lsid = makeLogicalSessionIdForTest();
    opCtx->setLogicalSessionId(lsid);

    const auto findCmd = BSON("find"
                              << "test"
                              << "batchSize" << 1);
    const CursorId cursorId = 1;

    RemoteCommandRequest rcr(HostAndPort("localhost"), "test", findCmd, opCtx.get());

    boost::optional<TaskExecutorCursor> tec;
    tec.emplace(makeTec(rcr, []() {
        TaskExecutorCursor::Options opts;
        opts.batchSize = 1;
        return opts;
    }()));

    // lsid in the first batch
    ASSERT_BSONOBJ_EQ(BSON("find"
                           << "test"
                           << "batchSize" << 1 << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulCursorResponse("firstBatch", 1, 1, cursorId));

    ASSERT_EQUALS(tec->getNext(opCtx.get()).value()["x"].Int(), 1);

    // lsid in the getmore
    ASSERT_BSONOBJ_EQ(BSON("getMore" << 1LL << "collection"
                                     << "test"
                                     << "batchSize" << 1 << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulCursorResponse("nextBatch", 2, 2, cursorId));

    tec.reset();

    // lsid in the killcursor
    ASSERT_BSONOBJ_EQ(BSON("killCursors"
                           << "test"
                           << "cursors" << BSON_ARRAY(1) << "lsid" << lsid.toBSON()),
                      scheduleSuccessfulKillCursorResponse(1));

    ASSERT_FALSE(hasReadyRequests());
}

}  // namespace
}  // namespace executor
}  // namespace mongo

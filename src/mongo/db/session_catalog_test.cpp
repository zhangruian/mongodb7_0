
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

#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/session_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class SessionCatalogTest : public MockReplCoordServerFixture {
protected:
    void setUp() final {
        MockReplCoordServerFixture::setUp();

        catalog()->reset_forTest();
    }

    SessionCatalog* catalog() {
        return SessionCatalog::get(getServiceContext());
    }
};

// When this class is in scope, makes the system behave as if we're in a DBDirectClient
class DirectClientSetter {
public:
    explicit DirectClientSetter(OperationContext* opCtx)
        : _opCtx(opCtx), _wasInDirectClient(_opCtx->getClient()->isInDirectClient()) {
        _opCtx->getClient()->setInDirectClient(true);
    }

    ~DirectClientSetter() {
        _opCtx->getClient()->setInDirectClient(_wasInDirectClient);
    }

private:
    const OperationContext* _opCtx;
    const bool _wasInDirectClient;
};

TEST_F(SessionCatalogTest, CheckoutAndReleaseSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    auto scopedSession = catalog()->checkOutSession(opCtx());

    ASSERT(scopedSession.get());
    ASSERT_EQ(*opCtx()->getLogicalSessionId(), scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, OperationContextCheckedOutSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    const TxnNumber txnNum = 20;
    opCtx()->setTxnNumber(txnNum);

    OperationContextSession ocs(opCtx(), true);
    auto session = OperationContextSession::get(opCtx());
    ASSERT(session);
    ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
}

TEST_F(SessionCatalogTest, OperationContextNonCheckedOutSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    OperationContextSession ocs(opCtx(), false);
    auto session = OperationContextSession::get(opCtx());

    ASSERT(!session);
}

TEST_F(SessionCatalogTest, GetOrCreateNonExistentSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    auto scopedSession = catalog()->getOrCreateSession(opCtx(), lsid);

    ASSERT(scopedSession.get());
    ASSERT_EQ(lsid, scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, GetOrCreateSessionAfterCheckOutSession) {
    const auto lsid = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid);

    boost::optional<OperationContextSession> ocs;
    ocs.emplace(opCtx(), true);

    stdx::async(stdx::launch::async, [&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();

    ocs.reset();

    stdx::async(stdx::launch::async, [&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        auto scopedSession =
            SessionCatalog::get(sideOpCtx.get())->getOrCreateSession(sideOpCtx.get(), lsid);

        ASSERT(scopedSession.get());
        ASSERT_EQ(lsid, scopedSession->getSessionId());
    }).get();
}

TEST_F(SessionCatalogTest, NestedOperationContextSession) {
    opCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        OperationContextSession outerScopedSession(opCtx(), true);

        {
            DirectClientSetter inDirectClient(opCtx());
            OperationContextSession innerScopedSession(opCtx(), true);

            auto session = OperationContextSession::get(opCtx());
            ASSERT(session);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
        }

        {
            DirectClientSetter inDirectClient(opCtx());
            auto session = OperationContextSession::get(opCtx());
            ASSERT(session);
            ASSERT_EQ(*opCtx()->getLogicalSessionId(), session->getSessionId());
        }
    }

    ASSERT(!OperationContextSession::get(opCtx()));
}

TEST_F(SessionCatalogTest, ScanSessions) {
    std::vector<LogicalSessionId> lsids;
    auto workerFn = [&](OperationContext* opCtx, Session* session) {
        lsids.push_back(session->getSessionId());
    };

    // Scan over zero Sessions.
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx())});
    catalog()->scanSessions(opCtx(), matcherAllSessions, workerFn);
    ASSERT(lsids.empty());

    // Create three sessions in the catalog.
    auto lsid1 = makeLogicalSessionIdForTest();
    auto lsid2 = makeLogicalSessionIdForTest();
    auto lsid3 = makeLogicalSessionIdForTest();
    {
        auto scopedSession1 = catalog()->getOrCreateSession(opCtx(), lsid1);
        auto scopedSession2 = catalog()->getOrCreateSession(opCtx(), lsid2);
        auto scopedSession3 = catalog()->getOrCreateSession(opCtx(), lsid3);
    }

    // Scan over all Sessions.
    lsids.clear();
    catalog()->scanSessions(opCtx(), matcherAllSessions, workerFn);
    ASSERT_EQ(lsids.size(), 3U);

    // Scan over all Sessions, visiting a particular Session.
    SessionKiller::Matcher matcherLSID2(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx(), lsid2)});
    lsids.clear();
    catalog()->scanSessions(opCtx(), matcherLSID2, workerFn);
    ASSERT_EQ(lsids.size(), 1U);
    ASSERT_EQ(lsids.front(), lsid2);
}

TEST_F(SessionCatalogTest, PreventCheckout) {
    const auto lsid = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid);
    opCtx()->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);

    {
        SessionCatalog::PreventCheckingOutSessionsBlock preventCheckoutBlock(catalog());

        ASSERT_THROWS_CODE(
            catalog()->checkOutSession(opCtx()), AssertionException, ErrorCodes::MaxTimeMSExpired);
    }

    auto scopedSession = catalog()->checkOutSession(opCtx());
    ASSERT(scopedSession.get());
    ASSERT_EQ(lsid, scopedSession->getSessionId());
}

TEST_F(SessionCatalogTest, WaitForAllSessions) {
    const auto lsid1 = makeLogicalSessionIdForTest();
    const auto lsid2 = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid1);

    // Check out a Session.
    boost::optional<OperationContextSession> ocs;
    ocs.emplace(opCtx(), true);
    ASSERT_EQ(lsid1, ocs->get(opCtx())->getSessionId());

    // Prevent new Sessions from being checked out.
    boost::optional<SessionCatalog::PreventCheckingOutSessionsBlock> preventCheckoutBlock;
    preventCheckoutBlock.emplace(catalog());

    // Enqueue a request to check out a Session.
    auto future = stdx::async(stdx::launch::async, [&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready();
        auto sideOpCtx = Client::getCurrent()->makeOperationContext();
        sideOpCtx->setLogicalSessionId(lsid2);
        auto asyncScopedSession =
            SessionCatalog::get(sideOpCtx.get())->checkOutSession(sideOpCtx.get());

        ASSERT(asyncScopedSession.get());
        ASSERT_EQ(lsid2, asyncScopedSession->getSessionId());
    });

    // Ensure that waitForAllSessionsToBeCheckedIn() times out since we are holding a Session
    // checked out.
    opCtx()->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);
    ASSERT_THROWS_CODE(preventCheckoutBlock->waitForAllSessionsToBeCheckedIn(opCtx()),
                       AssertionException,
                       ErrorCodes::MaxTimeMSExpired);

    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Release the Session we have checked out.
    ocs.reset();

    // Now ensure that waitForAllSessionsToBeCheckedIn() can complete.
    preventCheckoutBlock->waitForAllSessionsToBeCheckedIn(opCtx());

    // Ensure that the async thread trying to check out a Session is still blocked.
    ASSERT(stdx::future_status::ready != future.wait_for(Milliseconds(10).toSystemDuration()));

    // Allow checking out Sessions to proceed.
    preventCheckoutBlock.reset();

    // Ensure that the async thread can now proceed and successfully check out a Session.
    future.get();
}

TEST_F(SessionCatalogTest, MultiplePreventCheckingOutSessionsBlocks) {
    const auto lsid1 = makeLogicalSessionIdForTest();
    opCtx()->setLogicalSessionId(lsid1);
    opCtx()->setDeadlineAfterNowBy(Milliseconds(10), ErrorCodes::MaxTimeMSExpired);

    boost::optional<OperationContextSession> ocs;

    // Prevent new Sessions from being checked out.
    boost::optional<SessionCatalog::PreventCheckingOutSessionsBlock> preventCheckoutBlock1,
        preventCheckoutBlock2;
    preventCheckoutBlock1.emplace(catalog());

    // Ensure that checking out a Session fails
    ASSERT_THROWS_CODE(
        ocs.emplace(opCtx(), true), AssertionException, ErrorCodes::MaxTimeMSExpired);

    // A second request to prevent checking out Sessions is legal.
    preventCheckoutBlock2.emplace(catalog());
    ASSERT_THROWS_CODE(
        ocs.emplace(opCtx(), true), AssertionException, ErrorCodes::MaxTimeMSExpired);

    // The first request completing before the second is valid and doesn't start allowing checkouts.
    preventCheckoutBlock1.reset();
    ASSERT_THROWS_CODE(
        ocs.emplace(opCtx(), true), AssertionException, ErrorCodes::MaxTimeMSExpired);

    // Releasing the last PreventCheckingOutSessionsBlock allows Session checkout to proceed.
    preventCheckoutBlock2.reset();

    ASSERT_TRUE(ocs == boost::none);
    ocs.emplace(opCtx(), true);
    ASSERT_EQ(lsid1, ocs->get(opCtx())->getSessionId());
}

}  // namespace
}  // namespace mongo

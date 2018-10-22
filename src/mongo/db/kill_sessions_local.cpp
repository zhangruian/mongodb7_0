
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/kill_sessions_local.h"

#include "mongo/db/client.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

void killSessionsAction(OperationContext* opCtx,
                        const SessionKiller::Matcher& matcher,
                        const SessionCatalog::ScanSessionsCallbackFn& killSessionFn) {
    const auto catalog = SessionCatalog::get(opCtx);

    catalog->scanSessions(opCtx, matcher, [&](OperationContext* opCtx, Session* session) {
        // TODO (SERVER-33850): Rename KillAllSessionsByPattern and
        // ScopedKillAllSessionsByPatternImpersonator to not refer to session kill
        const KillAllSessionsByPattern* pattern = matcher.match(session->getSessionId());
        invariant(pattern);

        ScopedKillAllSessionsByPatternImpersonator impersonator(opCtx, *pattern);
        killSessionFn(opCtx, session);
    });
}

}  // namespace

void killSessionsLocalKillTransactions(OperationContext* opCtx,
                                       const SessionKiller::Matcher& matcher) {
    killSessionsAction(opCtx, matcher, [](OperationContext* opCtx, Session* session) {
        TransactionParticipant::getFromNonCheckedOutSession(session)->abortArbitraryTransaction();
    });
}

SessionKiller::Result killSessionsLocal(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher,
                                        SessionKiller::UniformRandomBitGenerator* urbg) {
    killSessionsLocalKillTransactions(opCtx, matcher);
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));

    auto res = CursorManager::killCursorsWithMatchingSessions(opCtx, matcher);
    uassertStatusOK(res.first);

    return {std::vector<HostAndPort>{}};
}

void killAllExpiredTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(opCtx, matcherAllSessions, [](OperationContext* opCtx, Session* session) {
        try {
            TransactionParticipant::getFromNonCheckedOutSession(session)
                ->abortArbitraryTransactionIfExpired();
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            std::string errmsg = str::stream()
                << "May have failed to abort expired transaction with session id (lsid) '"
                << session->getSessionId() << "'."
                << " Caused by: " << status;
            warning() << errmsg;
        }
    });
}

void killSessionsLocalShutdownAllTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(opCtx, matcherAllSessions, [](OperationContext* opCtx, Session* session) {
        TransactionParticipant::getFromNonCheckedOutSession(session)->shutdown();
    });
}

void killSessionsLocalAbortOrYieldAllTransactions(
    OperationContext* opCtx, std::vector<std::pair<Locker*, Locker::LockSnapshot>>* yieldedLocks) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    killSessionsAction(
        opCtx, matcherAllSessions, [yieldedLocks](OperationContext* opCtx, Session* session) {
            TransactionParticipant::getFromNonCheckedOutSession(session)
                ->abortOrYieldArbitraryTransaction(yieldedLocks);
        });
}

}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/split_prepare_session_manager.h"

namespace mongo {
namespace repl {

SplitPrepareSessionManager::SplitPrepareSessionManager(InternalSessionPool* sessionPool)
    : _sessionPool(sessionPool) {}

const std::vector<PooledSession>& SplitPrepareSessionManager::splitSession(
    const LogicalSessionId& sessionId, TxnNumber txnNumber, uint32_t numSplits) {
    invariant(numSplits > 0);
    stdx::lock_guard<Latch> lk(_mutex);

    auto [it, succ] =
        _splitSessionMap.try_emplace(sessionId, txnNumber, std::vector<PooledSession>());

    // The session must not be split before.
    invariant(succ);

    auto& sessions = it->second.second;
    sessions.reserve(numSplits);

    for (uint32_t i = 0; i < numSplits; ++i) {
        sessions.push_back(_sessionPool->acquireSystemSession());
    }

    return sessions;
}

boost::optional<const std::vector<PooledSession>&> SplitPrepareSessionManager::getSplitSessions(
    const LogicalSessionId& sessionId, TxnNumber txnNumber) const {
    stdx::lock_guard<Latch> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);
    if (it == _splitSessionMap.end()) {
        return boost::none;
    }

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    return it->second.second;
}

bool SplitPrepareSessionManager::isSessionSplit(const LogicalSessionId& sessionId,
                                                TxnNumber txnNumber) const {
    stdx::lock_guard<Latch> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);
    if (it == _splitSessionMap.end()) {
        return false;
    }

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    return true;
}

void SplitPrepareSessionManager::releaseSplitSessions(const LogicalSessionId& sessionId,
                                                      TxnNumber txnNumber) {
    stdx::lock_guard<Latch> lk(_mutex);

    auto it = _splitSessionMap.find(sessionId);

    // The session must already be split and tracked.
    invariant(it != _splitSessionMap.end());

    // The txnNumber must not change after the session was split.
    invariant(txnNumber == it->second.first);

    auto& sessions = it->second.second;
    for (const auto& sess : sessions) {
        _sessionPool->release(sess);
    }

    _splitSessionMap.erase(it);
}

}  // namespace repl
}  // namespace mongo

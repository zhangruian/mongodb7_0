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

#include "mongo/db/kill_sessions.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {

std::vector<KillAllSessionsUser> getKillAllSessionsImpersonateUsers(OperationContext* opCtx) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    std::vector<KillAllSessionsUser> out;

    for (auto iter = authSession->getAuthenticatedUserNames(); iter.more(); iter.next()) {
        out.emplace_back();
        out.back().setUser(iter->getUser());
        out.back().setDb(iter->getDB());
    }

    return out;
}

std::vector<KillAllSessionsRole> getKillAllSessionsImpersonateRoles(OperationContext* opCtx) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());

    std::vector<KillAllSessionsRole> out;

    for (auto iter = authSession->getAuthenticatedRoleNames(); iter.more(); iter.next()) {
        out.emplace_back();
        out.back().setRole(iter->getRole());
        out.back().setDb(iter->getDB());
    }

    return out;
}

}  // namespace

std::tuple<std::vector<UserName>, std::vector<RoleName>> getKillAllSessionsByPatternImpersonateData(
    const KillAllSessionsByPattern& pattern) {
    std::tuple<std::vector<UserName>, std::vector<RoleName>> out;

    auto& users = std::get<0>(out);
    auto& roles = std::get<1>(out);

    if (pattern.getUsers()) {
        users.reserve(pattern.getUsers()->size());

        for (auto&& user : pattern.getUsers().get()) {
            users.emplace_back(user.getUser(), user.getDb());
        }
    }

    if (pattern.getRoles()) {
        roles.reserve(pattern.getUsers()->size());

        for (auto&& user : pattern.getUsers().get()) {
            roles.emplace_back(user.getUser(), user.getDb());
        }
    }

    return out;
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx) {
    KillAllSessionsByPattern kasbp;

    kasbp.setUsers(getKillAllSessionsImpersonateUsers(opCtx));
    kasbp.setRoles(getKillAllSessionsImpersonateRoles(opCtx));
    return {kasbp, APIParameters::get(opCtx)};
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const KillAllSessionsUser& kasu) {
    KillAllSessionsByPatternItem item = makeKillAllSessionsByPattern(opCtx);

    auto authMgr = AuthorizationManager::get(opCtx->getServiceContext());

    UserName un(kasu.getUser(), kasu.getDb());

    auto user = uassertStatusOK(authMgr->acquireUser(opCtx, un));
    item.pattern.setUid(user->getDigest());
    return item;
}

KillAllSessionsByPatternSet makeSessionFilterForAuthenticatedUsers(OperationContext* opCtx) {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    KillAllSessionsByPatternSet patterns;

    for (auto it = authSession->getAuthenticatedUserNames(); it.more(); it.next()) {
        if (auto user = authSession->lookupUser(*it)) {
            KillAllSessionsByPattern pattern;
            pattern.setUid(user->getDigest());
            KillAllSessionsByPatternItem item{std::move(pattern), APIParameters::get(opCtx)};
            patterns.emplace(std::move(item));
        }
    }
    return patterns;
}

KillAllSessionsByPatternItem makeKillAllSessionsByPattern(OperationContext* opCtx,
                                                          const LogicalSessionId& lsid) {
    auto item = makeKillAllSessionsByPattern(opCtx);
    item.pattern.setLsid(lsid);
    return item;
}

}  // namespace mongo

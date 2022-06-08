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

#include "mongo/db/validated_tenant_id.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/security_token.h"
#include "mongo/db/multitenancy_gen.h"

namespace mongo {

ValidatedTenantId::ValidatedTenantId(const OpMsg& opMsg, Client& client) {
    auto dollarTenantElem = opMsg.body["$tenant"];
    uassert(ErrorCodes::InvalidOptions,
            "Multitenancy not enabled, cannot set $tenant in command body",
            !dollarTenantElem || (dollarTenantElem && gMultitenancySupport));

    if (!gMultitenancySupport) {
        return;
    }

    // TODO SERVER-66822: Re-enable this uassert.
    // uassert(ErrorCodes::Unauthorized,
    //         "Multitenancy is enabled, $tenant id or securityToken is required.",
    //         dollarTenantElem || opMsg.securityToken.nFields() > 0);

    uassert(6545800,
            str::stream() << "Cannot pass $tenant id if also passing securityToken, "
                          << opMsg.securityToken.toString() << ", " << dollarTenantElem.toString(),
            !(dollarTenantElem && opMsg.securityToken.nFields() > 0));

    if (dollarTenantElem) {
        uassert(ErrorCodes::Unauthorized,
                "'$tenant' may only be specified with the useTenant action type",
                AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::useTenant));
        _tenant = TenantId::parseFromBSON(dollarTenantElem);
        return;
    }

    if (opMsg.securityToken.nFields() > 0) {
        auto verifiedToken = auth::verifySecurityToken(opMsg.securityToken);
        _tenant = verifiedToken.getAuthenticatedUser().getTenant();
    }
}

ValidatedTenantId::ValidatedTenantId(const DatabaseName& dbName) {
    _tenant = dbName.tenantId();
}

}  // namespace mongo

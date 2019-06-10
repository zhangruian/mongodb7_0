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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authz_manager_external_state_d.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_session_external_state_d.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

AuthzManagerExternalStateMongod::AuthzManagerExternalStateMongod() = default;
AuthzManagerExternalStateMongod::~AuthzManagerExternalStateMongod() = default;

std::unique_ptr<AuthzSessionExternalState>
AuthzManagerExternalStateMongod::makeAuthzSessionExternalState(AuthorizationManager* authzManager) {
    return stdx::make_unique<AuthzSessionExternalStateMongod>(authzManager);
}
Status AuthzManagerExternalStateMongod::query(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const BSONObj& query,
    const BSONObj& projection,
    const std::function<void(const BSONObj&)>& resultProcessor) {
    try {
        DBDirectClient client(opCtx);
        client.query(resultProcessor, collectionName, query, &projection);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Status AuthzManagerExternalStateMongod::findOne(OperationContext* opCtx,
                                                const NamespaceString& collectionName,
                                                const BSONObj& query,
                                                BSONObj* result) {
    AutoGetCollectionForReadCommand ctx(opCtx, collectionName);

    BSONObj found;
    if (Helpers::findOne(opCtx, ctx.getCollection(), query, found)) {
        *result = found.getOwned();
        return Status::OK();
    }
    return Status(ErrorCodes::NoMatchingDocument,
                  str::stream() << "No document in " << collectionName.ns() << " matches "
                                << query);
}

MONGO_REGISTER_SHIM(AuthzManagerExternalState::create)
()->std::unique_ptr<AuthzManagerExternalState> {
    return std::make_unique<AuthzManagerExternalStateMongod>();
}

}  // namespace mongo

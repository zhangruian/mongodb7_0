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

#include "mongo/util/database_name_util.h"
#include "mongo/db/database_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/str.h"
#include <ostream>

#include "mongo/logv2/log_debug.h"

namespace mongo {

std::string DatabaseNameUtil::serialize(const DatabaseName& dbName,
                                        const SerializationContext& context) {
    if (!gMultitenancySupport) {
        return dbName.toString();
    }

    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
        return dbName.toString();
    }
    return dbName.toStringWithTenantId();
}

DatabaseName parseDbNameFromStringExpectTenantIdInMultitenancyMode(StringData dbName) {
    if (!gMultitenancySupport) {
        return DatabaseName(boost::none, dbName);
    }

    auto tenantDelim = dbName.find('_');
    if (tenantDelim == std::string::npos) {
        return DatabaseName(boost::none, dbName);
    }

    auto swOID = OID::parse(dbName.substr(0, tenantDelim));
    if (swOID.getStatus() == ErrorCodes::BadValue) {
        // If we fail to parse an OID, either the size of the substring is incorrect, or there is an
        // invalid character. This indicates that the db has the "_" character, but it does not act
        // as a delimeter for a tenantId prefix.
        return DatabaseName(boost::none, dbName);
    }

    const TenantId tenantId(swOID.getValue());
    return DatabaseName(tenantId, dbName.substr(tenantDelim + 1));
}

DatabaseName DatabaseNameUtil::deserialize(boost::optional<TenantId> tenantId,
                                           StringData db,
                                           const SerializationContext& context) {
    if (db.empty()) {
        return DatabaseName();
    }

    if (!gMultitenancySupport) {
        massert(7005302, "TenantId must not be set", tenantId == boost::none);
        return DatabaseName(boost::none, db);
    }

    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility)) {
        // TODO SERVER-62491 Remove this conditional, the tenantId should be kSystemTenantId.
        // TODO SERVER-73025 Uncomment out this conditional to check that we always have a tenantId.
        /* if (db != "admin" && db != "config" && db != "local")
            massert(7005300, "TenantId must be set", tenantId != boost::none);
        */

        return DatabaseName(std::move(tenantId), db);
    }

    auto dbName = parseDbNameFromStringExpectTenantIdInMultitenancyMode(db);
    // TenantId could be prefixed, or passed in separately (or both) and namespace is always
    // constructed with the tenantId separately.
    if (tenantId != boost::none) {
        if (!dbName.tenantId()) {
            return DatabaseName(std::move(tenantId), db);
        }
        massert(7005301, "TenantId must match that in db prefix", tenantId == dbName.tenantId());
    }
    return dbName;
}

}  // namespace mongo

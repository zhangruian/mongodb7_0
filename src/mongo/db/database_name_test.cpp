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

#include "mongo/db/database_name.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DatabaseNameTest, MultitenancySupportDisabled) {
    DatabaseName dbnWithoutTenant1(boost::none, "a");

    ASSERT(!dbnWithoutTenant1.tenantId());
    ASSERT_EQUALS(std::string("a"), dbnWithoutTenant1.db());
    ASSERT_EQUALS(std::string("a"), dbnWithoutTenant1.toString());

    TenantId tenantId(OID::gen());
    DatabaseName dbnWithTenant(tenantId, "a");
    ASSERT(dbnWithTenant.tenantId());
    ASSERT_EQUALS(tenantId, *dbnWithTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), dbnWithTenant.db());
    ASSERT_EQUALS(std::string("a"), dbnWithTenant.toString());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), dbnWithTenant.toStringWithTenantId());
}

TEST(DatabaseNameTest, MultitenancySupportEnabledTenantIDNotRequired) {
    // TODO SERVER-62114 remove this test case.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    DatabaseName dbnWithoutTenant(boost::none, "a");
    ASSERT(!dbnWithoutTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), dbnWithoutTenant.db());
    ASSERT_EQUALS(std::string("a"), dbnWithoutTenant.toString());

    TenantId tenantId(OID::gen());
    DatabaseName dbnWithTenant(tenantId, "a");
    ASSERT(dbnWithTenant.tenantId());
    ASSERT_EQUALS(tenantId, *dbnWithTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), dbnWithTenant.db());
    ASSERT_EQUALS(std::string("a"), dbnWithTenant.toString());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), dbnWithTenant.toStringWithTenantId());
}

/*
// TODO SERVER-65457 Re-enable these tests

DEATH_TEST(DatabaseNameTest, TenantIDRequiredNoTenantIdAssigned, "invariant") {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    DatabaseName dbnWithoutTenant(boost::none, "a");
}

TEST(DatabaseNameTest, TenantIDRequiredBasic) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    TenantId tenantId(OID::gen());
    DatabaseName dbn(tenantId, "a");
    ASSERT(dbn.tenantId());
    ASSERT_EQUALS(tenantId, *dbn.tenantId());
    ASSERT_EQUALS(std::string("a"), dbn.db());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), dbn.toString());
}
*/

TEST(DatabaseNameTest, VerifyEqualsOperator) {
    TenantId tenantId(OID::gen());
    DatabaseName dbn(tenantId, "a");
    ASSERT_TRUE(DatabaseName(tenantId, "a") == dbn);
    ASSERT_TRUE(DatabaseName(tenantId, "b") != dbn);

    TenantId otherTenantId = TenantId(OID::gen());
    ASSERT_TRUE(DatabaseName(otherTenantId, "a") != dbn);
    ASSERT_TRUE(DatabaseName(boost::none, "a") != dbn);
}

TEST(DatabaseNameTest, VerifyHashFunction) {
    TenantId tenantId1(OID::gen());
    TenantId tenantId2(OID::gen());
    DatabaseName dbn1 = DatabaseName(tenantId1, "a");
    DatabaseName dbn2 = DatabaseName(tenantId2, "a");
    DatabaseName dbn3 = DatabaseName(boost::none, "a");

    stdx::unordered_map<DatabaseName, std::string> dbMap;

    dbMap[dbn1] = "value T1 a1";
    ASSERT_EQUALS(dbMap[dbn1], "value T1 a1");
    dbMap[dbn1] = "value T1 a2";
    ASSERT_EQUALS(dbMap[dbn1], "value T1 a2");
    dbMap[DatabaseName(tenantId1, "a")] = "value T1 a3";
    ASSERT_EQUALS(dbMap[dbn1], "value T1 a3");

    dbMap[dbn2] = "value T2 a1";
    ASSERT_EQUALS(dbMap[dbn2], "value T2 a1");
    dbMap[dbn2] = "value T2 a2";

    dbMap[dbn3] = "value no tenant a1";
    ASSERT_EQUALS(dbMap[dbn3], "value no tenant a1");
    dbMap[dbn3] = "value no tenant a2";

    // verify all key-value in map to ensure all data is correct.
    ASSERT_EQUALS(dbMap[dbn1], "value T1 a3");
    ASSERT_EQUALS(dbMap[dbn2], "value T2 a2");
    ASSERT_EQUALS(dbMap[dbn3], "value no tenant a2");
}

TEST(DatabaseNameTest, VerifyCompareFunction) {
    TenantId tenantId1 = TenantId(OID::gen());
    TenantId tenantId2 = TenantId(OID::gen());

    // OID's generated by the same process are monotonically increasing.
    ASSERT(tenantId1 < tenantId2);

    DatabaseName dbn1a = DatabaseName(tenantId1, "a");
    DatabaseName dbn1b = DatabaseName(tenantId1, "b");
    DatabaseName dbn2a = DatabaseName(tenantId2, "a");
    DatabaseName dbn3a = DatabaseName(boost::none, "a");

    ASSERT(dbn1a < dbn1b);
    ASSERT(dbn1b < dbn2a);
    ASSERT(dbn3a != dbn1a);
    ASSERT(dbn1a != dbn2a);
}
}  // namespace
}  // namespace mongo

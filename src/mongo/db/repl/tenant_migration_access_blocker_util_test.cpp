/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class TenantMigrationAccessBlockerUtilTest : public ServiceContextTest {
public:
    const std::string kTenantId = "tenantId";
    const std::string kTenantDB = "tenantId_db";

    void setUp() {
        _opCtx = makeOperationContext();
    }

    void tearDown() {
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};


TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationInitiallyFalse) {
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithDonor) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeTrueWithDonor) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(donorMtab);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "anyDb"_sd));
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantId));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithRecipient) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationTrueWithBoth) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeTrueWithBoth) {
    auto uuid = UUID::gen();
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab = std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(donorMtab);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "anyDb"_sd));
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantId));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationDonorFalseForNoDbName) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeDonorFalseForNoDbName) {
    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(donorMtab);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeRecipientFalseForNoDbName) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), StringData()));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationFalseForUnrelatedDb) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "otherDb"_sd));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveTenantMigrationFalseAfterRemoveWithBoth) {
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));

    // Remove donor, should still be a migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kDonor);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));

    // Remove recipient, there should be no migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
}

TEST_F(TenantMigrationAccessBlockerUtilTest, HasActiveShardMergeFalseAfterRemoveWithBoth) {
    auto migrationId = UUID::gen();
    auto recipientMtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), migrationId);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, recipientMtab);

    auto donorMtab =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), migrationId);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(donorMtab);

    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "anyDb"_sd));

    // Remove donor, should still be a migration for the tenants migrating to the recipient.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .removeAccessBlockersForMigration(migrationId,
                                          TenantMigrationAccessBlocker::BlockerType::kDonor);
    ASSERT(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "anyDb"_sd));

    // Remove recipient, there should be no migration.
    TenantMigrationAccessBlockerRegistry::get(getServiceContext())
        .remove(kTenantId, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), kTenantDB));
    ASSERT_FALSE(tenant_migration_access_blocker::hasActiveTenantMigration(opCtx(), "anyDb"_sd));
}
}  // namespace mongo

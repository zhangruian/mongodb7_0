/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/logv2/log.h"
#include "mongo/util/database_name_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

namespace mongo {

namespace {

/**
 * kBlockAllTenantsKey must be unique across all possible tenant IDs.
 * Since the first four bytes of an OID are a unix epoch timestamp,
 * we can simply select a value prior to the inception of MongoDB,
 * and be guaranteed to never have a collision with a value
 * produced by OID::gen().
 */
const TenantId kBlockAllTenantsKey(
    OID("15650123"   /* unique timestamp */
        "0000000000" /* process id */
        "000000" /* counter */));
}  // namespace

using MtabType = TenantMigrationAccessBlocker::BlockerType;
using MtabPair = TenantMigrationAccessBlockerRegistry::DonorRecipientAccessBlockerPair;

const ServiceContext::Decoration<TenantMigrationAccessBlockerRegistry>
    TenantMigrationAccessBlockerRegistry::get =
        ServiceContext::declareDecoration<TenantMigrationAccessBlockerRegistry>();

TenantMigrationAccessBlockerRegistry::TenantMigrationAccessBlockerRegistry() {
    // Executor to asynchronously schedule blocking operations while the tenant migration access
    // blockers are in action. This provides migrated tenants isolation from the non-migrated
    // users. The executor is shared by all access blockers and the thread count goes to 0 when
    // there is no migration.
    ThreadPool::Options threadPoolOptions;
    threadPoolOptions.maxThreads = 4;
    // When there is no migration, reduce thread count to 0.
    threadPoolOptions.minThreads = 0;
    threadPoolOptions.threadNamePrefix = "TenantMigrationBlockerAsync-";
    threadPoolOptions.poolName = "TenantMigrationBlockerAsyncThreadPool";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    _asyncBlockingOperationsExecutor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface("TenantMigrationBlockerNet"));
}

void TenantMigrationAccessBlockerRegistry::add(const TenantId& tenantId,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtabType = mtab->getType();
    tassert(8423350,
            "Trying to add a multi-tenant migration donor blocker when this node already has a "
            "donor blocker for all tenants",
            mtabType != MtabType::kDonor || !_getAllTenantDonorAccessBlocker(lg));

    const auto& it = _tenantMigrationAccessBlockers.find(tenantId);
    if (it != _tenantMigrationAccessBlockers.end()) {
        auto existingMtab = it->second.getAccessBlocker(mtabType);
        if (existingMtab) {
            uasserted(ErrorCodes::ConflictingServerlessOperation,
                      str::stream() << "This node is already a "
                                    << (mtabType == MtabType::kDonor ? "donor" : "recipient")
                                    << " for tenantId \"" << tenantId << "\" with migrationId \""
                                    << existingMtab->getMigrationId().toString() << "\"");
        }
        // The migration protocol guarantees that the original donor node must be garbage collected
        // before it can be chosen as a recipient under the same tenant. Therefore, we only expect
        // to have both recipient and donor access blockers in the case of back-to-back migrations
        // where the node participates first as a recipient then a donor.
        invariant(mtabType == MtabType::kDonor);
        it->second.setAccessBlocker(std::move(mtab));
        return;
    }
    MtabPair mtabPair;
    mtabPair.setAccessBlocker(std::move(mtab));
    _tenantMigrationAccessBlockers.emplace(tenantId, mtabPair);
}

void TenantMigrationAccessBlockerRegistry::add(const std::vector<TenantId>& tenantIds,
                                               std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    for (auto&& tenantId : tenantIds) {
        add(tenantId, mtab);
    }
}

void TenantMigrationAccessBlockerRegistry::add(std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
    LOGV2_DEBUG(6114102, 1, "Adding donor access blocker for all tenants");
    stdx::lock_guard<Latch> lg(_mutex);
    const auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
    tassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "This node is already a donor with migrationId "
                          << donorAccessBlocker->getMigrationId().toString(),
            !donorAccessBlocker);

    const auto& foundDonorAccessBlocker =
        std::find_if(_tenantMigrationAccessBlockers.begin(),
                     _tenantMigrationAccessBlockers.end(),
                     [](const auto& pair) { return pair.second.getDonorAccessBlocker().get(); });
    uassert(
        ErrorCodes::ConflictingServerlessOperation,
        str::stream()
            << "Trying to add donor blocker for all tenants when this node already has a donor "
               "blocker for \""
            << foundDonorAccessBlocker->second.getDonorAccessBlocker()->getMigrationId().toString()
            << "\"",
        foundDonorAccessBlocker == _tenantMigrationAccessBlockers.end());


    MtabPair mtabPair;
    mtabPair.setAccessBlocker(std::move(mtab));
    _tenantMigrationAccessBlockers.emplace(kBlockAllTenantsKey, mtabPair);
}

void TenantMigrationAccessBlockerRegistry::_remove(WithLock,
                                                   const TenantId& tenantId,
                                                   MtabType type) {
    const auto& it = _tenantMigrationAccessBlockers.find(tenantId);

    if (it == _tenantMigrationAccessBlockers.end()) {
        return;
    }

    // Use a reference to the DonorRecipientAccessBlockerPair so that we manipulate the actual
    // value as opposed to a copy.
    auto& mtabPair = it->second;
    mtabPair.clearAccessBlocker(type);
    if (!mtabPair.getDonorAccessBlocker() && !mtabPair.getRecipientAccessBlocker()) {
        _tenantMigrationAccessBlockers.erase(it);
    }
}

void TenantMigrationAccessBlockerRegistry::remove(const TenantId& tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (type == MtabType::kDonor && _getAllTenantDonorAccessBlocker(lg)) {
        tasserted(8423348, "Using remove() for new-style donor access blocker");
    }

    _remove(lg, tenantId, type);
}

void TenantMigrationAccessBlockerRegistry::removeAccessBlockersForMigration(
    const UUID& migrationId, TenantMigrationAccessBlocker::BlockerType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (const auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
        type == MtabType::kDonor && donorAccessBlocker) {
        if (donorAccessBlocker->getMigrationId() != migrationId) {
            return;
        }
    }

    // Clear blockers for migrationId, and erase pairs with no blocker remaining.
    erase_if(_tenantMigrationAccessBlockers, [&](auto& it) {
        auto& mtabPair = it.second;
        auto blocker = mtabPair.getAccessBlocker(type);
        if (!blocker || blocker->getMigrationId() != migrationId) {
            return false;
        }

        mtabPair.clearAccessBlocker(type);
        MtabType oppositeType;
        switch (type) {
            case MtabType::kRecipient:
                oppositeType = MtabType::kDonor;
                break;
            case MtabType::kDonor:
                oppositeType = MtabType::kRecipient;
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return !mtabPair.getAccessBlocker(oppositeType);
    });
}

void TenantMigrationAccessBlockerRegistry::removeAll(MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg); donorAccessBlocker) {
        donorAccessBlocker->interrupt();
    }

    for (auto it = _tenantMigrationAccessBlockers.begin();
         it != _tenantMigrationAccessBlockers.end();) {
        _remove(lg, (it++)->first, type);
    }
}

boost::optional<MtabPair> TenantMigrationAccessBlockerRegistry::getAccessBlockersForDbName(
    const DatabaseName& dbName) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg, dbName);
    const auto tenantId = tenant_migration_access_blocker::extractTenantFromDatabaseName(dbName);

    if (!donorAccessBlocker && _tenantMigrationAccessBlockers.empty()) {
        return boost::none;
    }

    if (!tenantId) {
        if (donorAccessBlocker) {
            return MtabPair{donorAccessBlocker};
        }
        return boost::none;
    }

    const auto tid = TenantId::parseFromString(*tenantId);
    const auto& it = _tenantMigrationAccessBlockers.find(tid);
    if (donorAccessBlocker) {
        if (it != _tenantMigrationAccessBlockers.end()) {
            return MtabPair{donorAccessBlocker, it->second.getRecipientAccessBlocker()};
        }
        return MtabPair{donorAccessBlocker};
    }

    if (it != _tenantMigrationAccessBlockers.end()) {
        // Return a copy of the DonorRecipientAccessBlockerPair to the caller so that it
        // can be inspected and/or manipulated without changing the value in the registry.
        return it->second;
    }

    return boost::none;
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForDbName(
    const DatabaseName& dbName, MtabType type) {
    auto mtabPair = getAccessBlockersForDbName(dbName);
    if (!mtabPair) {
        return nullptr;
    }

    return mtabPair->getAccessBlocker(type);
}

std::shared_ptr<TenantMigrationDonorAccessBlocker>
TenantMigrationAccessBlockerRegistry::_getAllTenantDonorAccessBlocker(WithLock) const {
    const auto& it = _tenantMigrationAccessBlockers.find(kBlockAllTenantsKey);
    if (it == _tenantMigrationAccessBlockers.end()) {
        return nullptr;
    }

    return checked_pointer_cast<TenantMigrationDonorAccessBlocker>(
        it->second.getDonorAccessBlocker());
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getAccessBlockerForMigration(
    const UUID& migrationId, TenantMigrationAccessBlocker::BlockerType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    auto mtab =
        std::find_if(_tenantMigrationAccessBlockers.begin(),
                     _tenantMigrationAccessBlockers.end(),
                     [type, migrationId](const auto& pair) {
                         return pair.second.getAccessBlocker(type)->getMigrationId() == migrationId;
                     });

    if (mtab == _tenantMigrationAccessBlockers.end()) {
        return nullptr;
    }

    return mtab->second.getAccessBlocker(type);
}

std::shared_ptr<TenantMigrationDonorAccessBlocker>
TenantMigrationAccessBlockerRegistry::_getAllTenantDonorAccessBlocker(
    WithLock lk, const DatabaseName& dbName) const {
    // No-op oplog entries, e.g. for linearizable reads, use namespace "".
    bool isInternal = (dbName.db() == "" || NamespaceString(dbName).isOnInternalDb());
    if (isInternal) {
        return nullptr;
    }

    return _getAllTenantDonorAccessBlocker(lk);
}

std::shared_ptr<TenantMigrationAccessBlocker>
TenantMigrationAccessBlockerRegistry::getTenantMigrationAccessBlockerForTenantId(
    const boost::optional<TenantId>& tenantId, MtabType type) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
        type == MtabType::kDonor && donorAccessBlocker) {
        return donorAccessBlocker;
    }

    // TODO (SERVER-72213) We only need a boost::optional here due to common donor code for both
    // tenant migration and shard merge (in which case the tenantId might be empty).
    if (tenantId) {
        const auto& it = _tenantMigrationAccessBlockers.find(*tenantId);
        if (it != _tenantMigrationAccessBlockers.end()) {
            return it->second.getAccessBlocker(type);
        }
    }

    return nullptr;
}

void TenantMigrationAccessBlockerRegistry::applyAll(TenantMigrationAccessBlocker::BlockerType type,
                                                    applyAllCallback&& callback) {
    stdx::lock_guard<Latch> lg(_mutex);
    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        if (auto mtab = mtabPair.getAccessBlocker(type)) {
            callback(tenantId, mtab);
        }
    }
}

void TenantMigrationAccessBlockerRegistry::startup() {
    _asyncBlockingOperationsExecutor->startup();
}

void TenantMigrationAccessBlockerRegistry::clear() {
    stdx::lock_guard<Latch> lg(_mutex);
    _clear(lg);
}

void TenantMigrationAccessBlockerRegistry::_clear(WithLock) {
    _tenantMigrationAccessBlockers.clear();
}

void TenantMigrationAccessBlockerRegistry::shutDown() {
    stdx::lock_guard<Latch> lg(_mutex);
    _clear(lg);
    _asyncBlockingOperationsExecutor.reset();
}

void TenantMigrationAccessBlockerRegistry::appendInfoForServerStatus(
    BSONObjBuilder* builder) const {
    stdx::lock_guard<Latch> lg(_mutex);

    const auto globalDonorAccessBlocker = _getAllTenantDonorAccessBlocker(lg);
    if (globalDonorAccessBlocker) {
        BSONObjBuilder donorMtabInfoBuilder;
        globalDonorAccessBlocker->appendInfoForServerStatus(&donorMtabInfoBuilder);
        builder->append("donor", donorMtabInfoBuilder.obj());
    }

    for (auto& [tenantId, mtabPair] : _tenantMigrationAccessBlockers) {
        BSONObjBuilder mtabInfoBuilder;

        auto donorMtab = mtabPair.getDonorAccessBlocker() ? mtabPair.getDonorAccessBlocker()
                                                          : globalDonorAccessBlocker;
        if (donorMtab) {
            BSONObjBuilder donorMtabInfoBuilder;
            donorMtab->appendInfoForServerStatus(&donorMtabInfoBuilder);
            mtabInfoBuilder.append("donor", donorMtabInfoBuilder.obj());
        }

        if (auto recipientMtab = mtabPair.getRecipientAccessBlocker()) {
            BSONObjBuilder recipientMtabInfoBuilder;
            recipientMtab->appendInfoForServerStatus(&recipientMtabInfoBuilder);
            mtabInfoBuilder.append("recipient", recipientMtabInfoBuilder.obj());
        }

        if (mtabInfoBuilder.len()) {
            builder->append(tenantId.toString(), mtabInfoBuilder.obj());
        }
    }
}

void TenantMigrationAccessBlockerRegistry::onMajorityCommitPointUpdate(repl::OpTime opTime) {
    stdx::lock_guard<Latch> lg(_mutex);

    for (auto& [_, mtabPair] : _tenantMigrationAccessBlockers) {
        if (auto recipientMtab = mtabPair.getRecipientAccessBlocker()) {
            recipientMtab->onMajorityCommitPointUpdate(opTime);
        }
        if (auto donorMtab = mtabPair.getDonorAccessBlocker()) {
            donorMtab->onMajorityCommitPointUpdate(opTime);
        }
    }

    if (auto donorAccessBlocker = _getAllTenantDonorAccessBlocker(lg); donorAccessBlocker) {
        donorAccessBlocker->onMajorityCommitPointUpdate(opTime);
    }
}

std::shared_ptr<executor::TaskExecutor>
TenantMigrationAccessBlockerRegistry::getAsyncBlockingOperationsExecutor() const {
    return _asyncBlockingOperationsExecutor;
}

}  // namespace mongo

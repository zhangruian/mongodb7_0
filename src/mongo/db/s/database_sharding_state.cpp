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

#include "mongo/db/s/database_sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class DatabaseShardingStateMap {
    DatabaseShardingStateMap& operator=(const DatabaseShardingStateMap&) = delete;
    DatabaseShardingStateMap(const DatabaseShardingStateMap&) = delete;

public:
    static const ServiceContext::Decoration<DatabaseShardingStateMap> get;

    DatabaseShardingStateMap() {}

    struct DSSAndLock {
        DSSAndLock(const DatabaseName& dbName)
            : dssMutex("DSSMutex::" + dbName.db()),
              dss(std::make_unique<DatabaseShardingState>(dbName)) {}

        const Lock::ResourceMutex dssMutex;
        std::unique_ptr<DatabaseShardingState> dss;
    };

    DSSAndLock* getOrCreate(const DatabaseName& dbName) {
        stdx::lock_guard<Latch> lg(_mutex);

        auto it = _databases.find(dbName);
        if (it == _databases.end()) {
            auto inserted = _databases.try_emplace(dbName, std::make_unique<DSSAndLock>(dbName));
            invariant(inserted.second);
            it = std::move(inserted.first);
        }

        return it->second.get();
    }

    std::vector<DatabaseName> getDatabaseNames() {
        stdx::lock_guard lg(_mutex);
        std::vector<DatabaseName> result;
        result.reserve(_databases.size());
        for (const auto& [dbName, _] : _databases) {
            result.emplace_back(dbName);
        }
        return result;
    }

private:
    Mutex _mutex = MONGO_MAKE_LATCH("DatabaseShardingStateMap::_mutex");

    // Entries of the _databases map must never be deleted or replaced. This is to guarantee that a
    // 'dbName' is always associated to the same 'ResourceMutex'.
    using DatabasesMap = stdx::unordered_map<DatabaseName, std::unique_ptr<DSSAndLock>>;
    DatabasesMap _databases;
};

const ServiceContext::Decoration<DatabaseShardingStateMap> DatabaseShardingStateMap::get =
    ServiceContext::declareDecoration<DatabaseShardingStateMap>();

}  // namespace

DatabaseShardingState::ScopedDatabaseShardingState::ScopedDatabaseShardingState(
    Lock::ResourceLock lock, DatabaseShardingState* dss)
    : _lock(std::move(lock)), _dss(dss) {}


DatabaseShardingState::ScopedDatabaseShardingState::ScopedDatabaseShardingState(
    ScopedDatabaseShardingState&& other)
    : _lock(std::move(other._lock)), _dss(other._dss) {
    other._dss = nullptr;
}

DatabaseShardingState::ScopedDatabaseShardingState::~ScopedDatabaseShardingState() = default;

DatabaseShardingState::DatabaseShardingState(const DatabaseName& dbName) : _dbName(dbName) {}

DatabaseShardingState::ScopedDatabaseShardingState DatabaseShardingState::assertDbLockedAndAcquire(
    OperationContext* opCtx, const DatabaseName& dbName, DSSAcquisitionMode mode) {
    dassert(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IS));

    return acquire(opCtx, dbName, mode);
}

DatabaseShardingState::ScopedDatabaseShardingState DatabaseShardingState::acquire(
    OperationContext* opCtx, const DatabaseName& dbName, DSSAcquisitionMode mode) {
    DatabaseShardingStateMap::DSSAndLock* dssAndLock =
        DatabaseShardingStateMap::get(opCtx->getServiceContext()).getOrCreate(dbName);

    // First lock the RESOURCE_MUTEX associated to this dbName to guarantee stability of the
    // DatabaseShardingState pointer. After that, it is safe to get and store the
    // DatabaseShadingState*, as long as the RESOURCE_MUTEX is kept locked.
    Lock::ResourceLock lock(opCtx->lockState(),
                            dssAndLock->dssMutex.getRid(),
                            mode == DSSAcquisitionMode::kShared ? MODE_IS : MODE_X);

    return ScopedDatabaseShardingState(std::move(lock), dssAndLock->dss.get());
}

std::vector<DatabaseName> DatabaseShardingState::getDatabaseNames(OperationContext* opCtx) {
    auto& databasesMap = DatabaseShardingStateMap::get(opCtx->getServiceContext());
    return databasesMap.getDatabaseNames();
}

void DatabaseShardingState::enterCriticalSectionCatchUpPhase(OperationContext* opCtx,
                                                             const BSONObj& reason) {
    _critSec.enterCriticalSectionCatchUpPhase(reason);
    cancelDbMetadataRefresh();
}

void DatabaseShardingState::enterCriticalSectionCommitPhase(OperationContext* opCtx,
                                                            const BSONObj& reason) {
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void DatabaseShardingState::exitCriticalSection(OperationContext* opCtx, const BSONObj& reason) {
    _critSec.exitCriticalSection(reason);
}

void DatabaseShardingState::exitCriticalSectionNoChecks(OperationContext* opCtx) {
    _critSec.exitCriticalSectionNoChecks();
}

void DatabaseShardingState::setMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_X));
    _movePrimaryInProgress = true;
}

void DatabaseShardingState::unsetMovePrimaryInProgress(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(_dbName, MODE_IX));
    _movePrimaryInProgress = false;
}

void DatabaseShardingState::setDbMetadataRefreshFuture(SharedSemiFuture<void> future,
                                                       CancellationSource cancellationSource) {
    invariant(!_dbMetadataRefresh);
    _dbMetadataRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>> DatabaseShardingState::getDbMetadataRefreshFuture() const {
    return _dbMetadataRefresh ? boost::optional<SharedSemiFuture<void>>(_dbMetadataRefresh->future)
                              : boost::none;
}

void DatabaseShardingState::resetDbMetadataRefreshFuture() {
    _dbMetadataRefresh = boost::none;
}

void DatabaseShardingState::cancelDbMetadataRefresh() {
    if (_dbMetadataRefresh) {
        _dbMetadataRefresh->cancellationSource.cancel();
    }
}

}  // namespace mongo

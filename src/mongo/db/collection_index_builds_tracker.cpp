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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/collection_index_builds_tracker.h"

#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/logv2/log.h"

namespace mongo {

CollectionIndexBuildsTracker::~CollectionIndexBuildsTracker() {
    invariant(_buildStateByBuildUUID.empty());
    invariant(_buildStateByIndexName.empty());
}

void CollectionIndexBuildsTracker::addIndexBuild(
    WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    // Ensure that a new entry is added.
    invariant(
        _buildStateByBuildUUID.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    invariant(replIndexBuildState->indexNames.size());
    for (auto& indexName : replIndexBuildState->indexNames) {
        // Duplicate index names within the same index build are ignored here because these will
        // be caught during validation by the IndexCatalog.
        _buildStateByIndexName.emplace(indexName, replIndexBuildState);
    }
}

void CollectionIndexBuildsTracker::removeIndexBuild(
    WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    invariant(_buildStateByBuildUUID.find(replIndexBuildState->buildUUID) !=
              _buildStateByBuildUUID.end());
    _buildStateByBuildUUID.erase(replIndexBuildState->buildUUID);

    for (const auto& indexName : replIndexBuildState->indexNames) {
        _buildStateByIndexName.erase(indexName);
    }

    _indexBuildFinishedCondVar.notify_all();
}

std::shared_ptr<ReplIndexBuildState> CollectionIndexBuildsTracker::getIndexBuildState(
    WithLock, StringData indexName) const {
    auto it = _buildStateByIndexName.find(indexName.toString());
    invariant(it != _buildStateByIndexName.end());
    return it->second;
}

bool CollectionIndexBuildsTracker::hasIndexBuildState(WithLock, StringData indexName) const {
    auto it = _buildStateByIndexName.find(indexName.toString());
    if (it == _buildStateByIndexName.end()) {
        return false;
    }
    return true;
}

std::vector<UUID> CollectionIndexBuildsTracker::getIndexBuildUUIDs(WithLock) const {
    std::vector<UUID> buildUUIDs;
    for (const auto& state : _buildStateByBuildUUID) {
        buildUUIDs.push_back(state.first);
    }
    return buildUUIDs;
}

void CollectionIndexBuildsTracker::runOperationOnAllBuilds(
    WithLock lk,
    IndexBuildsManager* indexBuildsManager,
    std::function<void(WithLock,
                       IndexBuildsManager* indexBuildsManager,
                       std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                       const std::string& reason)> func,
    const std::string& reason) noexcept {
    for (auto it = _buildStateByBuildUUID.begin(); it != _buildStateByBuildUUID.end(); ++it) {
        func(lk, indexBuildsManager, it->second, reason);
    }
}

int CollectionIndexBuildsTracker::getNumberOfIndexBuilds(WithLock) const {
    return _buildStateByBuildUUID.size();
}

void CollectionIndexBuildsTracker::waitUntilNoIndexBuildsRemain(stdx::unique_lock<Latch>& lk) {
    _indexBuildFinishedCondVar.wait(lk, [&] {
        if (_buildStateByBuildUUID.empty()) {
            return true;
        }

        LOGV2(20425, "Waiting until the following index builds are finished:");
        for (const auto& indexBuild : _buildStateByBuildUUID) {
            LOGV2(20426,
                  "    Index build with UUID: {indexBuild_first}",
                  "indexBuild_first"_attr = indexBuild.first);
        }

        return false;
    });
}

void CollectionIndexBuildsTracker::waitUntilIndexBuildFinished(stdx::unique_lock<Latch>& lk,
                                                               const UUID& buildUUID) {
    LOGV2(23867,
          "Waiting until index build with UUID {buildUUID} is finished",
          "buildUUID"_attr = buildUUID);

    _indexBuildFinishedCondVar.wait(lk, [&] {
        if (_buildStateByBuildUUID.find(buildUUID) == _buildStateByBuildUUID.end()) {
            return true;
        }
        return false;
    });
}

}  // namespace mongo

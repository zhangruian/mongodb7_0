/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/persistent_task_store.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using MigrationUtilsTest = ShardServerTestFixture;

RangeDeletionTask createDeletionTask(NamespaceString nss, const UUID& uuid, int min, int max) {
    RangeDeletionTask task{nss, uuid, CleanWhenEnum::kNow};

    task.setRange(ChunkRange{BSON("_id" << min), BSON("_id" << max)});

    return task;
}

// Test that overlappingRangeQuery() can handle the cases that we expect to encounter.
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
//                          |---------O                Range 1 [25, 35)
//      |---------O                                    Range 2 [5, 15)
//           |---------O                               Range 4 [10, 20)
// |----O                                              Range 5 [0, 5)
//             |-----O                                 Range 7 [12, 18)
//                               |---------O           Range 8 [30, 40)
// Ranges in store
// |---------O                                         [0, 10)
//           |---------O                               [10, 20)
//                                         |---------O [40 50)
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
TEST_F(MigrationUtilsTest, TestOverlappingRangeQuery) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();

    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx, createDeletionTask(NamespaceString{"one"}, uuid, 0, 10));
    store.add(opCtx, createDeletionTask(NamespaceString{"two"}, uuid, 10, 20));
    store.add(opCtx, createDeletionTask(NamespaceString{"three"}, uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    // 1. Non-overlapping range
    auto range1 = ChunkRange{BSON("_id" << 25), BSON("_id" << 35)};
    auto results = store.query(opCtx, migrationutil::overlappingRangeQuery(range1, uuid));
    ASSERT_EQ(results.size(), 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range2, uuid));
    ASSERT_EQ(results.size(), 2);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << 10), BSON("_id" << 20)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range4, uuid));
    ASSERT_EQ(results.size(), 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << 0), BSON("_id" << 5)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range5, uuid));
    ASSERT_EQ(results.size(), 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << 5), BSON("_id" << 10)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range6, uuid));
    ASSERT_EQ(results.size(), 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << 12), BSON("_id" << 18)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range7, uuid));
    ASSERT_EQ(results.size(), 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << 30), BSON("_id" << 40)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range8, uuid));
    ASSERT_EQ(results.size(), 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << 20), BSON("_id" << 30)};
    results = store.query(opCtx, migrationutil::overlappingRangeQuery(range9, uuid));
    ASSERT_EQ(results.size(), 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(MigrationUtilsTest, TestInvalidUUID) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();

    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx, createDeletionTask(NamespaceString{"one"}, uuid, 0, 10));
    store.add(opCtx, createDeletionTask(NamespaceString{"two"}, uuid, 10, 20));
    store.add(opCtx, createDeletionTask(NamespaceString{"three"}, uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    const auto wrongUuid = UUID::gen();
    auto range = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    auto results = store.query(opCtx, migrationutil::overlappingRangeQuery(range, wrongUuid));
    ASSERT_EQ(results.size(), 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range, wrongUuid));
}

}  // namespace
}  // namespace mongo
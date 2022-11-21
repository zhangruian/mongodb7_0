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

#include "mongo/bson/oid.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/ce/scalar_histogram.h"
#include "mongo/db/query/ce/stats_cache_loader_impl.h"
#include "mongo/db/query/ce/stats_cache_loader_test_fixture.h"
#include "mongo/db/query/ce/stats_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

class StatsCacheLoaderTest : public StatsCacheLoaderTestFixture {
protected:
    void createStatsCollection(NamespaceString nss);
    StatsCacheLoaderImpl _statsCacheLoader;
};

void StatsCacheLoaderTest::createStatsCollection(NamespaceString nss) {
    auto opCtx = operationContext();
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);
    auto db = autoColl.ensureDbExists(opCtx);
    WriteUnitOfWork wuow(opCtx);
    ASSERT(db->createCollection(opCtx, nss));
    wuow.commit();
}

TEST_F(StatsCacheLoaderTest, VerifyStatsLoad) {
    // Initialize histogram buckets.
    constexpr double doubleCount = 15.0;
    constexpr double trueCount = 12.0;
    constexpr double falseCount = 16.0;
    constexpr double numDocs = doubleCount + trueCount + falseCount;
    std::vector<ce::Bucket> buckets{
        ce::Bucket{1.0, 0.0, 1.0, 0.0, 1.0},
        ce::Bucket{2.0, 5.0, 8.0, 1.0, 2.0},
        ce::Bucket{3.0, 4.0, 15.0, 2.0, 6.0},
    };

    // Initialize histogram bounds.
    auto [boundsTag, boundsVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard boundsGuard{boundsTag, boundsVal};
    auto bounds = sbe::value::getArrayView(boundsVal);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 1.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 2.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 3.0);

    // Create a scalar histogram.
    ce::TypeCounts tc{
        {sbe::value::TypeTags::NumberDouble, doubleCount},
        {sbe::value::TypeTags::Boolean, trueCount + falseCount},
    };
    ce::ScalarHistogram sh(*bounds, buckets);
    ce::ArrayHistogram ah(sh, tc, trueCount, falseCount);
    auto expectedSerialized = ah.serialize();

    // Serialize histogram into a stats path.
    std::string path = "somePath";
    auto serialized = stats::makeStatsPath(path, numDocs, ah);

    // Initalize stats collection.
    NamespaceString nss("test", "stats");
    std::string statsColl(StatsCacheLoader::kStatsPrefix + "." + nss.coll());
    NamespaceString statsNss(nss.db(), statsColl);
    createStatsCollection(statsNss);

    // Write serialized stats path to collection.
    AutoGetCollection autoColl(operationContext(), statsNss, MODE_IX);
    const CollectionPtr& coll = autoColl.getCollection();
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(collection_internal::insertDocument(
            operationContext(), coll, InsertStatement(serialized), nullptr));
        wuow.commit();
    }

    // Read stats path & verify values are consistent with what we expect.
    auto actualAH = _statsCacheLoader.getStats(operationContext(), std::make_pair(nss, path)).get();
    auto actualSerialized = actualAH->serialize();

    ASSERT_BSONOBJ_EQ(expectedSerialized, actualSerialized);
}

}  // namespace
}  // namespace mongo

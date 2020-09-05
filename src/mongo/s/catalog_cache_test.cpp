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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/stale_exception.h"

namespace mongo {
namespace {

class CatalogCacheTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        // Setup dummy config server
        setRemote(kConfigHostAndPort);
        configTargeter()->setFindHostReturnValue(kConfigHostAndPort);

        // Setup catalogCache with mock loader
        _catalogCacheLoader = std::make_unique<CatalogCacheLoaderMock>();
        _catalogCache = std::make_unique<CatalogCache>(getServiceContext(), *_catalogCacheLoader);

        // Populate the shardRegistry with the shards from kShards vector
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        for (const auto& shardId : kShards) {
            shardInfos.emplace_back(
                std::make_tuple(shardId, HostAndPort(shardId.toString(), kDummyPort)));
        }
        addRemoteShards(shardInfos);
    };

    void loadDatabases(const std::vector<DatabaseType>& databases) {
        for (const auto& db : databases) {
            _catalogCacheLoader->setDatabaseRefreshReturnValue(db);
            const auto swDatabase = _catalogCache->getDatabase(operationContext(), db.getName());
            ASSERT_OK(swDatabase.getStatus());
        }

        // Reset the database return value to avoid false positive results
        _catalogCacheLoader->setDatabaseRefreshReturnValue(kErrorStatus);
    }

    void loadCollection(const ChunkVersion& version) {
        const auto coll = makeCollectionType(version);
        _catalogCacheLoader->setCollectionRefreshReturnValue(coll);
        _catalogCacheLoader->setChunkRefreshReturnValue(makeChunks(version));

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfo(operationContext(), coll.getNs());
        ASSERT_OK(swChunkManager.getStatus());

        // Reset the loader return values to avoid false positive results
        _catalogCacheLoader->setCollectionRefreshReturnValue(kErrorStatus);
        _catalogCacheLoader->setChunkRefreshReturnValue(kErrorStatus);
    }

    void loadUnshardedCollection(const NamespaceString& nss) {
        _catalogCacheLoader->setCollectionRefreshReturnValue(
            Status(ErrorCodes::NamespaceNotFound, "collection not found"));

        const auto swChunkManager =
            _catalogCache->getCollectionRoutingInfo(operationContext(), nss);
        ASSERT_OK(swChunkManager.getStatus());

        // Reset the loader return value to avoid false positive results
        _catalogCacheLoader->setCollectionRefreshReturnValue(kErrorStatus);
    }

    std::vector<ChunkType> makeChunks(ChunkVersion version) {
        ChunkType chunk(kNss,
                        {kShardKeyPattern.getKeyPattern().globalMin(),
                         kShardKeyPattern.getKeyPattern().globalMax()},
                        version,
                        {"0"});
        chunk.setName(OID::gen());
        return {chunk};
    }

    CollectionType makeCollectionType(const ChunkVersion& collVersion) {
        CollectionType coll;
        coll.setNs(kNss);
        coll.setEpoch(collVersion.epoch());
        coll.setKeyPattern(kShardKeyPattern.getKeyPattern());
        coll.setUnique(false);
        return coll;
    }

    const NamespaceString kNss{"catalgoCacheTestDB.foo"};
    const std::string kPattern{"_id"};
    const ShardKeyPattern kShardKeyPattern{BSON(kPattern << 1)};
    const int kDummyPort{12345};
    const HostAndPort kConfigHostAndPort{"DummyConfig", kDummyPort};
    const std::vector<ShardId> kShards{{"0"}, {"1"}};
    const Status kErrorStatus{ErrorCodes::InternalError,
                              "Received an unexpected CatalogCacheLoader request"};

    std::unique_ptr<CatalogCacheLoaderMock> _catalogCacheLoader;
    std::unique_ptr<CatalogCache> _catalogCache;
};

TEST_F(CatalogCacheTest, GetDatabase) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), 1);
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(dbName, kShards[0], true, dbVersion));

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_TRUE(cachedDb.shardingEnabled());
    ASSERT_EQ(cachedDb.primaryId(), kShards[0]);
    ASSERT_EQ(cachedDb.databaseVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb.databaseVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, GetCachedDatabase) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), 1);
    loadDatabases({DatabaseType(dbName, kShards[0], true, dbVersion)});

    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    const auto cachedDb = swDatabase.getValue();
    ASSERT_TRUE(cachedDb.shardingEnabled());
    ASSERT_EQ(cachedDb.primaryId(), kShards[0]);
    ASSERT_EQ(cachedDb.databaseVersion().getUuid(), dbVersion.getUuid());
    ASSERT_EQ(cachedDb.databaseVersion().getLastMod(), dbVersion.getLastMod());
}

TEST_F(CatalogCacheTest, InvalidateSingleDbOnShardRemoval) {
    const auto dbName = "testDB";
    const auto dbVersion = DatabaseVersion(UUID::gen(), 1);
    loadDatabases({DatabaseType(dbName, kShards[0], true, dbVersion)});

    _catalogCache->invalidateEntriesThatReferenceShard(kShards[0]);
    _catalogCacheLoader->setDatabaseRefreshReturnValue(
        DatabaseType(dbName, kShards[1], true, dbVersion));
    const auto swDatabase = _catalogCache->getDatabase(operationContext(), dbName);

    ASSERT_OK(swDatabase.getStatus());
    auto cachedDb = swDatabase.getValue();
    ASSERT_EQ(cachedDb.primaryId(), kShards[1]);
}

TEST_F(CatalogCacheTest, CheckEpochNoDatabase) {
    const auto collVersion = ChunkVersion(1, 0, OID::gen());
    ASSERT_THROWS_WITH_CHECK(_catalogCache->checkEpochOrThrow(kNss, collVersion, kShards[0]),
                             StaleConfigException,
                             [&](const StaleConfigException& ex) {
                                 const auto staleInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT(staleInfo);
                                 ASSERT_EQ(staleInfo->getNss(), kNss);
                                 ASSERT_EQ(staleInfo->getVersionReceived(), collVersion);
                                 ASSERT_EQ(staleInfo->getShardId(), kShards[0]);
                                 ASSERT(staleInfo->getVersionWanted() == boost::none);
                             });
}

TEST_F(CatalogCacheTest, CheckEpochNoCollection) {
    const auto dbVersion = DatabaseVersion();
    const auto collVersion = ChunkVersion(1, 0, OID::gen());

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    ASSERT_THROWS_WITH_CHECK(_catalogCache->checkEpochOrThrow(kNss, collVersion, kShards[0]),
                             StaleConfigException,
                             [&](const StaleConfigException& ex) {
                                 const auto staleInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT(staleInfo);
                                 ASSERT_EQ(staleInfo->getNss(), kNss);
                                 ASSERT_EQ(staleInfo->getVersionReceived(), collVersion);
                                 ASSERT_EQ(staleInfo->getShardId(), kShards[0]);
                                 ASSERT(staleInfo->getVersionWanted() == boost::none);
                             });
}

TEST_F(CatalogCacheTest, CheckEpochUnshardedCollection) {
    const auto dbVersion = DatabaseVersion();
    const auto collVersion = ChunkVersion(1, 0, OID::gen());

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadUnshardedCollection(kNss);
    ASSERT_THROWS_WITH_CHECK(_catalogCache->checkEpochOrThrow(kNss, collVersion, kShards[0]),
                             StaleConfigException,
                             [&](const StaleConfigException& ex) {
                                 const auto staleInfo = ex.extraInfo<StaleConfigInfo>();
                                 ASSERT(staleInfo);
                                 ASSERT_EQ(staleInfo->getNss(), kNss);
                                 ASSERT_EQ(staleInfo->getVersionReceived(), collVersion);
                                 ASSERT_EQ(staleInfo->getShardId(), kShards[0]);
                                 ASSERT(staleInfo->getVersionWanted() == boost::none);
                             });
}

TEST_F(CatalogCacheTest, CheckEpochWithMismatch) {
    const auto dbVersion = DatabaseVersion();
    const auto wantedCollVersion = ChunkVersion(1, 0, OID::gen());
    const auto receivedCollVersion = ChunkVersion(1, 0, OID::gen());

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadCollection(wantedCollVersion);

    ASSERT_THROWS_WITH_CHECK(
        _catalogCache->checkEpochOrThrow(kNss, receivedCollVersion, kShards[0]),
        StaleConfigException,
        [&](const StaleConfigException& ex) {
            const auto staleInfo = ex.extraInfo<StaleConfigInfo>();
            ASSERT(staleInfo);
            ASSERT_EQ(staleInfo->getNss(), kNss);
            ASSERT_EQ(staleInfo->getVersionReceived(), receivedCollVersion);
            ASSERT(staleInfo->getVersionWanted() != boost::none);
            ASSERT_EQ(*(staleInfo->getVersionWanted()), wantedCollVersion);
            ASSERT_EQ(staleInfo->getShardId(), kShards[0]);
        });
}

TEST_F(CatalogCacheTest, CheckEpochWithMatch) {
    const auto dbVersion = DatabaseVersion();
    const auto collVersion = ChunkVersion(1, 0, OID::gen());

    loadDatabases({DatabaseType(kNss.db().toString(), kShards[0], true, dbVersion)});
    loadCollection(collVersion);

    _catalogCache->checkEpochOrThrow(kNss, collVersion, kShards[0]);
}

}  // namespace
}  // namespace mongo

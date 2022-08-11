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

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/index/index_build_interceptor.h"

namespace mongo {
namespace {
class IndexBuilderInterceptorTest : public CatalogTestFixture {
protected:
    const IndexCatalogEntry* createIndex(BSONObj spec) {
        WriteUnitOfWork wuow(operationContext());
        auto* indexCatalog = _coll->getWritableCollection(operationContext())->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            operationContext(), _coll->getWritableCollection(operationContext()), spec));
        wuow.commit();

        return indexCatalog->getEntry(indexCatalog->findIndexByName(
            operationContext(), spec.getStringField(IndexDescriptor::kIndexNameFieldName)));
    }

    std::unique_ptr<IndexBuildInterceptor> createIndexBuildInterceptor(BSONObj spec) {
        return std::make_unique<IndexBuildInterceptor>(operationContext(),
                                                       createIndex(std::move(spec)));
    }

    std::unique_ptr<TemporaryRecordStore> getSideWritesTable(
        std::unique_ptr<IndexBuildInterceptor> interceptor) {
        // In order to get access to the interceptor's side writes table, we have to mark the table
        // as permanent and then destroy the interceptor.
        interceptor->keepTemporaryTables();
        auto sideWritesIdent = interceptor->getSideWritesTableIdent();
        interceptor.reset();

        return operationContext()
            ->getServiceContext()
            ->getStorageEngine()
            ->makeTemporaryRecordStoreFromExistingIdent(operationContext(), sideWritesIdent);
    }

    std::vector<BSONObj> getSideWritesTableContents(
        std::unique_ptr<IndexBuildInterceptor> interceptor) {
        auto table = getSideWritesTable(std::move(interceptor));

        std::vector<BSONObj> contents;
        auto cursor = table->rs()->getCursor(operationContext());
        while (auto record = cursor->next()) {
            contents.push_back(record->data.toBson().getOwned());
        }
        return contents;
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        _coll.emplace(operationContext(), _nss, MODE_X);
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

private:
    NamespaceString _nss{"testDB.interceptor"};
    boost::optional<AutoGetCollection> _coll;
};

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSideWritesTable) {
    auto interceptor = createIndexBuildInterceptor(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));

    KeyString::HeapBuilder ksBuilder(KeyString::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    KeyString::Value keyString(ksBuilder.release());

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), {keyString}, {}, {}, IndexBuildInterceptor::Op::kInsert, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BufBuilder bufBuilder;
    keyString.serialize(bufBuilder);
    BSONBinData serializedKeyString(bufBuilder.buf(), bufBuilder.len(), BinDataGeneral);

    auto sideWrites = getSideWritesTableContents(std::move(interceptor));
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("op"
                           << "i"
                           << "key" << serializedKeyString),
                      sideWrites[0]);
}
}  // namespace
}  // namespace mongo

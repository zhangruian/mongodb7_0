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

#include "mongo/platform/basic.h"

#include "mongo/db/change_collection_expired_documents_remover.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_collection_truncate_markers.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "pipeline/change_stream_test_helpers.h"

namespace mongo {

class ChangeCollectionExpiredChangeRemoverTest : public CatalogTestFixture {
protected:
    ChangeCollectionExpiredChangeRemoverTest()
        : CatalogTestFixture(Options{}.useMockClock(true)),
          _tenantId(change_stream_serverless_helpers::getTenantIdForTesting()) {
        ChangeStreamChangeCollectionManager::create(getServiceContext());
    }

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    Date_t now() {
        return clockSource()->now();
    }

    void insertDocumentToChangeCollection(OperationContext* opCtx,
                                          const TenantId& tenantId,
                                          const BSONObj& obj) {
        const auto wallTime = now();
        Timestamp timestamp{wallTime};

        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpTime(repl::OpTime{timestamp, 0});
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(NamespaceString::makeChangeCollectionNSS(tenantId));
        oplogEntry.setObject(obj);
        oplogEntry.setWallClockTime(wallTime);

        auto oplogEntryBson = oplogEntry.toBSON();

        RecordData recordData{oplogEntryBson.objdata(), oplogEntryBson.objsize()};
        RecordId recordId =
            record_id_helpers::keyForOptime(timestamp, KeyFormat::String).getValue();

        AutoGetChangeCollection changeCollection{
            opCtx, AutoGetChangeCollection::AccessMode::kWrite, tenantId};

        WriteUnitOfWork wunit(opCtx);
        ChangeStreamChangeCollectionManager::get(opCtx).insertDocumentsToChangeCollection(
            opCtx, {Record{recordId, recordData}}, {timestamp});
        wunit.commit();
    }

    std::vector<repl::OplogEntry> readChangeCollection(OperationContext* opCtx,
                                                       boost::optional<TenantId> tenantId) {
        auto changeCollection =
            AutoGetChangeCollection{opCtx, AutoGetChangeCollection::AccessMode::kRead, tenantId};

        auto scanExecutor =
            InternalPlanner::collectionScan(opCtx,
                                            &(*changeCollection),
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            InternalPlanner::Direction::FORWARD);

        BSONObj currChangeDoc;
        std::vector<repl::OplogEntry> entries;
        while (scanExecutor->getNext(&currChangeDoc, nullptr) == PlanExecutor::ADVANCED) {
            entries.push_back(repl::OplogEntry(currChangeDoc));
        }

        return entries;
    }

    void dropAndRecreateChangeCollection(OperationContext* opCtx, const TenantId& tenantId) {
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.dropChangeCollection(opCtx, tenantId);
        changeCollectionManager.createChangeCollection(opCtx, tenantId);
    }

    size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                   boost::optional<TenantId> tenantId,
                                                   Date_t expirationTime) {
        // Acquire intent-exclusive lock on the change collection. Early exit if the collection
        // doesn't exist.
        const auto changeCollection =
            AutoGetChangeCollection{opCtx, AutoGetChangeCollection::AccessMode::kWrite, tenantId};

        // Get the 'maxRecordIdBound' and perform the removal of the expired documents.
        const auto maxRecordIdBound =
            ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(
                opCtx, &*changeCollection)
                ->maxRecordIdBound;
        return ChangeStreamChangeCollectionManager::removeExpiredChangeCollectionsDocuments(
            opCtx, &*changeCollection, maxRecordIdBound, expirationTime);
    }

    const TenantId _tenantId;
    boost::optional<ChangeStreamChangeCollectionManager> _changeCollectionManager;

    RAIIServerParameterControllerForTest featureFlagController{"featureFlagServerlessChangeStreams",
                                                               true};
    RAIIServerParameterControllerForTest queryKnobController{
        "internalChangeStreamUseTenantIdForTesting", true};
};

class ChangeCollectionTruncateExpirationTest : public ChangeCollectionExpiredChangeRemoverTest {
protected:
    ChangeCollectionTruncateExpirationTest() : ChangeCollectionExpiredChangeRemoverTest() {}

    void setExpireAfterSeconds(OperationContext* opCtx, Seconds seconds) {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* changeStreamsParam =
            clusterParameters
                ->get<ClusterParameterWithStorage<ChangeStreamsClusterParameterStorage>>(
                    "changeStreams");

        auto oldSettings = changeStreamsParam->getValue(_tenantId);
        oldSettings.setExpireAfterSeconds(seconds.count());
        changeStreamsParam->setValue(oldSettings, _tenantId).ignore();
    }

    void insertDocumentToChangeCollection(OperationContext* opCtx,
                                          const TenantId& tenantId,
                                          const BSONObj& obj) {
        WriteUnitOfWork wuow(opCtx);
        ChangeCollectionExpiredChangeRemoverTest::insertDocumentToChangeCollection(
            opCtx, tenantId, obj);
        const auto wallTime = now();
        Timestamp timestamp{wallTime};
        RecordId recordId =
            record_id_helpers::keyForOptime(timestamp, KeyFormat::String).getValue();

        _truncateMarkers->updateCurrentMarkerAfterInsertOnCommit(
            opCtx, obj.objsize(), recordId, wallTime, 1);
        wuow.commit();
    }

    void dropAndRecreateChangeCollection(OperationContext* opCtx,
                                         const TenantId& tenantId,
                                         int64_t minBytesPerMarker) {
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.dropChangeCollection(opCtx, tenantId);
        _truncateMarkers.reset();
        changeCollectionManager.createChangeCollection(opCtx, tenantId);
        _truncateMarkers = std::make_unique<ChangeCollectionTruncateMarkers>(
            tenantId, std::deque<CollectionTruncateMarkers::Marker>{}, 0, 0, minBytesPerMarker);
    }

    size_t removeExpiredChangeCollectionsDocuments(OperationContext* opCtx,
                                                   boost::optional<TenantId> tenantId,
                                                   Date_t expirationTime) {
        // Acquire intent-exclusive lock on the change collection. Early exit if the collection
        // doesn't exist.
        const auto changeCollection =
            AutoGetChangeCollection{opCtx, AutoGetChangeCollection::AccessMode::kWrite, tenantId};

        WriteUnitOfWork wuow(opCtx);
        size_t numRecordsDeleted = 0;
        while (boost::optional<CollectionTruncateMarkers::Marker> marker =
                   _truncateMarkers->peekOldestMarkerIfNeeded(opCtx)) {
            auto recordStore = changeCollection->getRecordStore();

            ASSERT_OK(recordStore->rangeTruncate(
                opCtx, RecordId(), marker->lastRecord, -marker->bytes, -marker->records));

            _truncateMarkers->popOldestMarker();
            numRecordsDeleted += marker->records;
        }
        wuow.commit();
        return numRecordsDeleted;
    }

    std::unique_ptr<ChangeCollectionTruncateMarkers> _truncateMarkers;
};

// Tests that the last expired focument retrieved is the expected one.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, VerifyLastExpiredDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    Date_t expirationTime;
    BSONObj lastExpiredDocument;

    // Create 100 change documents and consider the first 50 of them as expired.
    for (int i = 0; i < 100; ++i) {
        auto doc = BSON("_id" << i);
        insertDocumentToChangeCollection(opCtx, _tenantId, doc);

        // Store the last document and it's wallTime.
        if (i == 99) {
            lastExpiredDocument = doc;
            expirationTime = now();
        }

        clockSource()->advance(Milliseconds(1));
    }

    auto changeCollection =
        AutoGetChangeCollection{opCtx, AutoGetChangeCollection::AccessMode::kRead, _tenantId};

    auto maxExpiredRecordId =
        ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(
            opCtx, &*changeCollection)
            ->maxRecordIdBound;

    // Get the document found at 'maxExpiredRecordId' and test it against 'lastExpiredDocument'.
    auto scanExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        &(*changeCollection),
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none,
                                        maxExpiredRecordId,
                                        maxExpiredRecordId);

    BSONObj changeDocAtId;
    ASSERT_EQ(scanExecutor->getNext(&changeDocAtId, nullptr), PlanExecutor::ADVANCED);
    ASSERT_BSONOBJ_EQ(repl::OplogEntry(changeDocAtId).getObject(), lastExpiredDocument);
}

// Tests that only the expired documents are removed from the change collection.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, ShouldRemoveOnlyExpiredDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    const BSONObj firstExpired = BSON("_id"
                                      << "firstExpired");
    const BSONObj secondExpired = BSON("_id"
                                       << "secondExpired");
    const BSONObj notExpired = BSON("_id"
                                    << "notExpired");

    insertDocumentToChangeCollection(opCtx, _tenantId, firstExpired);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, secondExpired);

    // Store the wallTime of the last expired document.
    const auto expirationTime = now();
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, notExpired);

    // Verify that only the required documents are removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, expirationTime), 2);

    // Only the 'notExpired' document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), notExpired);
}

// Tests that the last expired document is never deleted.
TEST_F(ChangeCollectionExpiredChangeRemoverTest, ShouldLeaveAtLeastOneDocument) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId);

    for (int i = 0; i < 100; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << i));
        clockSource()->advance(Milliseconds{1});
    }

    // Verify that all but the last document is removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, now()), 99);

    // Only the last document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), BSON("_id" << 99));
}

// Tests that only the expired documents are removed from the change collection.
TEST_F(ChangeCollectionTruncateExpirationTest, ShouldRemoveOnlyExpiredDocument_Markers) {
    const BSONObj firstExpired = BSON("_id"
                                      << "firstExpired");
    const BSONObj secondExpired = BSON("_id"
                                       << "secondExpired");
    const BSONObj notExpired = BSON("_id"
                                    << "notExpired");

    const auto timeAtStart = now();
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(
        opCtx, _tenantId, firstExpired.objsize() + secondExpired.objsize());

    insertDocumentToChangeCollection(opCtx, _tenantId, firstExpired);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, secondExpired);

    // Store the wallTime of the last expired document.
    const auto expirationTime = now();
    const auto expirationTimeInSeconds = duration_cast<Seconds>(expirationTime - timeAtStart);
    setExpireAfterSeconds(opCtx, expirationTimeInSeconds);
    clockSource()->advance(Hours(1));
    insertDocumentToChangeCollection(opCtx, _tenantId, notExpired);

    // Verify that only the required documents are removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, expirationTime), 2);

    // Only the 'notExpired' document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), notExpired);
}

// Tests that the last expired document is never deleted.
TEST_F(ChangeCollectionTruncateExpirationTest, ShouldLeaveAtLeastOneDocument_Markers) {
    const auto opCtx = operationContext();
    dropAndRecreateChangeCollection(opCtx, _tenantId, 1);

    setExpireAfterSeconds(opCtx, Seconds{1});

    for (int i = 0; i < 100; ++i) {
        insertDocumentToChangeCollection(opCtx, _tenantId, BSON("_id" << i));
        clockSource()->advance(Seconds{1});
    }

    // Verify that all but the last document is removed.
    ASSERT_EQ(removeExpiredChangeCollectionsDocuments(opCtx, _tenantId, now()), 99);

    // Only the last document is left in the change collection.
    const auto changeCollectionEntries = readChangeCollection(opCtx, _tenantId);
    ASSERT_EQ(changeCollectionEntries.size(), 1);
    ASSERT_BSONOBJ_EQ(changeCollectionEntries[0].getObject(), BSON("_id" << 99));
}
}  // namespace mongo

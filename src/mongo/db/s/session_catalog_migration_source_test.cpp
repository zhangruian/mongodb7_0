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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const NamespaceString kNs("a.b");
const KeyPattern kShardKey(BSON("x" << 1));
const ChunkRange kChunkRange(BSON("x" << 0), BSON("x" << 100));
const KeyPattern kNestedShardKey(BSON("x.y" << 1));
const ChunkRange kNestedChunkRange(BSON("x.y" << 0), BSON("x.y" << 100));

class SessionCatalogMigrationSourceTest : public MockReplCoordServerFixture {};

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                Date_t wallClockTime,
                                const std::vector<StmtId>& stmtIds,
                                repl::OpTime prevWriteOpTimeInTransaction,
                                boost::optional<repl::OpTime> preImageOpTime,
                                boost::optional<repl::OpTime> postImageOpTime,
                                boost::optional<OperationSessionInfo> osi,
                                boost::optional<repl::RetryImageEnum> needsRetryImage) {
    return repl::DurableOplogEntry(
        opTime,                           // optime
        0,                                // hash
        opType,                           // opType
        nss,                              // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        repl::OplogEntry::kOplogVersion,  // version
        object,                           // o
        object2,                          // o2
        osi.get_value_or({}),             // sessionInfo
        boost::none,                      // upsert
        wallClockTime,                    // wall clock time
        stmtIds,                          // statement ids
        prevWriteOpTimeInTransaction,     // optime of previous write within same transaction
        preImageOpTime,                   // pre-image optime
        postImageOpTime,                  // post-image optime
        boost::none,                      // ShardId of resharding recipient
        boost::none,                      // _id
        needsRetryImage);
}

repl::OplogEntry makeOplogEntry(
    repl::OpTime opTime,
    repl::OpTypeEnum opType,
    BSONObj object,
    boost::optional<BSONObj> object2,
    Date_t wallClockTime,
    const std::vector<StmtId>& stmtIds,
    repl::OpTime prevWriteOpTimeInTransaction,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none,
    boost::optional<OperationSessionInfo> osi = boost::none,
    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
    return makeOplogEntry(opTime,
                          opType,
                          kNs,
                          object,
                          object2,
                          wallClockTime,
                          stmtIds,
                          prevWriteOpTimeInTransaction,
                          preImageOpTime,
                          postImageOpTime,
                          osi,
                          needsRetryImage);
}

repl::OplogEntry makeSentinelOplogEntry(const LogicalSessionId& lsid,
                                        const TxnNumber& txnNumber,
                                        Date_t wallClockTime) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    return makeOplogEntry({},                                        // optime
                          repl::OpTypeEnum::kNoop,                   // op type
                          {},                                        // o
                          TransactionParticipant::kDeadEndSentinel,  // o2
                          wallClockTime,                             // wall clock time
                          {kIncompleteHistoryStmtId},                // statement id
                          repl::OpTime(Timestamp(0, 0), 0),
                          boost::none,
                          boost::none,
                          sessionInfo  // session info
    );
}

repl::OplogEntry makeRewrittenOplogInSession(repl::OpTime opTime,
                                             repl::OpTime previousWriteOpTime,
                                             BSONObj object,
                                             int statementId) {
    auto original =
        makeOplogEntry(opTime,                     // optime
                       repl::OpTypeEnum::kInsert,  // op type
                       object,                     // o
                       boost::none,                // o2
                       Date_t::now(),              // wall clock time
                       {statementId},              // statement ids
                       previousWriteOpTime);  // optime of previous write within same transaction

    return makeOplogEntry(original.getOpTime(),                                         // optime
                          repl::OpTypeEnum::kNoop,                                      // op type
                          BSON(SessionCatalogMigration::kSessionMigrateOplogTag << 1),  // o
                          original.getEntry().toBSON(),                                 // o2
                          original.getWallClockTime(),  // wall clock time
                          original.getStatementIds(),   // statement ids
                          original.getPrevWriteOpTimeInTransaction()
                              .get());  // optime of previous write within same transaction
};


TEST_F(SessionCatalogMigrationSourceTest, NoSessionsToTransferShouldNotHaveOplog) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(0, migrationSource.untransferredCatchUpDataSize());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWrites) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       {1},                                    // statement ids
                       entry1.getOpTime());  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithTwoWritesMultiStmtIds) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0, 1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       {2, 3},                                 // statement ids
                       entry1.getOpTime());  // optime of previous write within same transaction
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWrites) {
    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << 50),                        // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       {1},                                    // statement ids
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        {3},                                 // statement ids
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time
                       {4},                                  // statement ids
                       entry2a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(entry2b.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto checkNextBatch = [this, &migrationSource](const repl::OplogEntry& firstExpectedOplog,
                                                   const repl::OplogEntry& secondExpectedOplog) {
        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            // Cannot compare directly because of SERVER-31356
            ASSERT_BSONOBJ_EQ(firstExpectedOplog.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
            ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        }

        {
            ASSERT_TRUE(migrationSource.hasMoreOplog());
            auto nextOplogResult = migrationSource.getLastFetchedOplog();
            ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
            ASSERT_BSONOBJ_EQ(secondExpectedOplog.getEntry().toBSON(),
                              nextOplogResult.oplog->getEntry().toBSON());
        }
    };

    if (sessionRecord1.getSessionId().toBSON().woCompare(sessionRecord2.getSessionId().toBSON()) <
        0) {
        checkNextBatch(entry2b, entry2a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry1b, entry1a);

    } else {
        checkNextBatch(entry1b, entry1a);

        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        checkNextBatch(entry2b, entry2a);
    }
}

// It is currently not possible to have 2 findAndModify operations in one transaction, but this
// will test the oplog buffer more.
TEST_F(SessionCatalogMigrationSourceTest, OneSessionWithFindAndModifyPreImageAndPostImage) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(73, 5), 2),  // optime
        repl::OpTypeEnum::kNoop,            // op type
        BSON("x" << 20),                    // o
        boost::none,                        // o2
        Date_t::now(),                      // wall clock time
        {2},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry3);

    auto entry4 =
        makeOplogEntry(repl::OpTime(Timestamp(73, 6), 2),  // optime
                       repl::OpTypeEnum::kUpdate,          // op type
                       BSON("$inc" << BSON("x" << 1)),     // o
                       BSON("x" << 19),                    // o2
                       Date_t::now(),                      // wall clock time
                       {3},                                // statement ids
                       entry2.getOpTime(),   // optime of previous write within same transaction
                       boost::none,          // pre-image optime
                       entry3.getOpTime());  // post-image optime
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry3, entry4, entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       OneSessionWithFindAndModifyPreImageAndPostImageMultiStmtIds) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0, 1},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {2, 3},                               // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(73, 5), 2),  // optime
        repl::OpTypeEnum::kNoop,            // op type
        BSON("x" << 20),                    // o
        boost::none,                        // o2
        Date_t::now(),                      // wall clock time
        {4, 5},                             // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
    insertOplogEntry(entry3);

    auto entry4 =
        makeOplogEntry(repl::OpTime(Timestamp(73, 6), 2),  // optime
                       repl::OpTypeEnum::kUpdate,          // op type
                       BSON("$inc" << BSON("x" << 1)),     // o
                       BSON("x" << 19),                    // o2
                       Date_t::now(),                      // wall clock time
                       {6, 7},                             // statement ids
                       entry2.getOpTime(),   // optime of previous write within same transaction
                       boost::none,          // pre-image optime
                       entry3.getOpTime());  // post-image optime
    insertOplogEntry(entry4);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry4.getOpTime());
    sessionRecord.setLastWriteDate(entry4.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry3, entry4, entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ForgeImageEntriesWhenFetchingEntriesWithNeedsRetryImage) {
    repl::ImageEntry imageEntry;
    const auto preImage = BSON("_id" << 1 << "x" << 50);
    const auto lsid = makeLogicalSessionIdForTest();
    const repl::OpTime imageEntryOpTime = repl::OpTime(Timestamp(52, 346), 2);
    const auto txnNumber = 1LL;
    imageEntry.set_id(lsid);
    imageEntry.setTxnNumber(txnNumber);
    imageEntry.setTs(imageEntryOpTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);

    OperationSessionInfo osi;
    osi.setTxnNumber(txnNumber);
    osi.setSessionId(lsid);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntry.toBSON());

    // Insert an oplog entry with a non-null needsRetryImage field.
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement id
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        boost::none,                          // post-image optime
        osi,                                  // operation session info
        repl::RetryImageEnum::kPreImage);     // needsRetryImage
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(lsid);
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    // The next oplog entry should be the forged preImage entry.
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Check that the key fields are what we expect. The destination will overwrite any unneeded
    // fields when it processes the incoming entries.
    ASSERT_BSONOBJ_EQ(preImage, nextOplogResult.oplog->getObject());
    ASSERT_EQUALS(txnNumber, nextOplogResult.oplog->getTxnNumber().get());
    ASSERT_EQUALS(lsid, nextOplogResult.oplog->getSessionId().get());
    ASSERT_EQUALS("n", repl::OpType_serializer(nextOplogResult.oplog->getOpType()));
    ASSERT_EQUALS(0LL, nextOplogResult.oplog->getStatementIds().front());

    // The next oplog entry should be the original entry that generated the image entry.
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
}

TEST_F(SessionCatalogMigrationSourceTest, OplogWithOtherNsShouldBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());


    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(53, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        NamespaceString("x.y"),              // namespace
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        {1},                                 // statement ids
        repl::OpTime(Timestamp(0, 0), 0),    // optime of previous write within same transaction
        boost::none,                         // pre-image optime
        boost::none,                         // post-image optime
        boost::none,                         // operation session info
        boost::none);                        // needsRetryImage
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord2.setLastWriteDate(entry2.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, SessionDumpWithMultipleNewWrites) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    insertOplogEntry(entry1);

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1.getOpTime());
    sessionRecord1.setLastWriteDate(entry1.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(53, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        {1},                                 // statement ids
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
    insertOplogEntry(entry2);

    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(55, 12), 2),  // optime
        repl::OpTypeEnum::kInsert,           // op type
        BSON("x" << 40),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        {2},                                 // statement ids
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
    insertOplogEntry(entry3);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(
        entry2.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);
    migrationSource.notifyNewWriteOpTime(
        entry3.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry1.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry2.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry3.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertIfOplogCannotBeFound) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    migrationSource.notifyNewWriteOpTime(
        repl::OpTime(Timestamp(100, 3), 1),
        SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldBeAbleInsertNewWritesAfterBufferWasDepleted) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(52, 345), 2),  // optime
            repl::OpTypeEnum::kInsert,            // op type
            BSON("x" << 30),                      // o
            boost::none,                          // o2
            Date_t::now(),                        // wall clock time
            {0},                                  // statement ids
            repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(53, 12), 2),  // optime
            repl::OpTypeEnum::kDelete,           // op type
            BSON("x" << 30),                     // o
            boost::none,                         // o2
            Date_t::now(),                       // wall clock time
            {1},                                 // statement ids
            repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }

    {
        auto entry = makeOplogEntry(
            repl::OpTime(Timestamp(55, 12), 2),  // optime
            repl::OpTypeEnum::kInsert,           // op type
            BSON("x" << 40),                     // o
            boost::none,                         // o2
            Date_t::now(),                       // wall clock time
            {2},                                 // statement ids
            repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction
        insertOplogEntry(entry);

        migrationSource.notifyNewWriteOpTime(
            entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

        ASSERT_TRUE(migrationSource.hasMoreOplog());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_TRUE(nextOplogResult.shouldWaitForMajority);
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());

        ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_FALSE(migrationSource.hasMoreOplog());
    }
}

TEST_F(SessionCatalogMigrationSourceTest, ReturnsDeadEndSentinelForIncompleteHistory) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(40, 1), 2));   // optime of previous write within same transaction
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    }

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);

        auto oplog = *nextOplogResult.oplog;
        ASSERT_TRUE(oplog.getObject2());
        ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel, *oplog.getObject2());
        ASSERT_EQ(1, oplog.getStatementIds().size());
        ASSERT_EQ(kIncompleteHistoryStmtId, oplog.getStatementIds().front());
        ASSERT_NE(Date_t{}, oplog.getWallClockTime());

        auto sessionInfo = oplog.getOperationSessionInfo();
        ASSERT_TRUE(sessionInfo.getSessionId());
        ASSERT_EQ(sessionId, *sessionInfo.getSessionId());
        ASSERT_TRUE(sessionInfo.getTxnNumber());
        ASSERT_EQ(31, *sessionInfo.getTxnNumber());
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldAssertWhenRollbackDetected) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(40, 1), 2));   // optime of previous write within same transaction
    insertOplogEntry(entry);

    const auto sessionId = makeLogicalSessionIdForTest();

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(sessionId);
    sessionRecord.setTxnNum(31);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(entry.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
    }

    ASSERT_OK(repl::ReplicationProcess::get(opCtx())->incrementRollbackID(opCtx()));

    ASSERT_THROWS(migrationSource.fetchNextOplog(opCtx()), AssertionException);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       CommitTransactionEntriesShouldBeConvertedToDeadEndSentinel) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kCommitted);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());

    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                      *nextOplogResult.oplog->getObject2());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       PrepareTransactionEntriesShouldBeConvertedToDeadEndSentinel) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kPrepared);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.hasMoreOplog());

    auto nextOplogResult = migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
    // Cannot compare directly because of SERVER-31356
    ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                      *nextOplogResult.oplog->getObject2());

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, InProgressTransactionEntriesShouldBeIgnored) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kInProgress);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, AbortedTransactionEntriesShouldBeIgnored) {
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(20);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 5));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kAborted);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       MixedTransactionEntriesAndRetryableWritesEntriesReturnCorrectResults) {
    // Create an entry corresponding to a retryable write.
    auto insertOplog = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    // Create a config.transaction entry pointing to the insert oplog entry.
    SessionTxnRecord retryableWriteRecord;
    retryableWriteRecord.setSessionId(makeLogicalSessionIdForTest());
    retryableWriteRecord.setTxnNum(1);
    retryableWriteRecord.setLastWriteOpTime(insertOplog.getOpTime());
    retryableWriteRecord.setLastWriteDate(insertOplog.getWallClockTime());

    // Create a config.transaction entry pointing to an imaginary commitTransaction entry.
    SessionTxnRecord txnRecord;
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(1);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(12, 34), 2));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setState(DurableTxnStateEnum::kCommitted);

    // Insert both entries into the config.transactions table.
    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  retryableWriteRecord.toBSON());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), txnRecord.toBSON());

    // Insert the 'insert' oplog entry into the oplog.
    insertOplogEntry(insertOplog);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    // Function to verify the oplog entry corresponding to the retryable write.
    auto checkRetryableWriteEntry = [&] {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(insertOplog.getEntry().toBSON(),
                          nextOplogResult.oplog->getEntry().toBSON());
    };

    // Function to verify the oplog entry corresponding to the transaction.
    auto checkTxnEntry = [&] {
        ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(TransactionParticipant::kDeadEndSentinel,
                          *nextOplogResult.oplog->getObject2());
    };

    // Logical session ids are generated randomly and the migration source queries in order of
    // logical session id, so we need to change the order of the checks depending on the ordering of
    // the lsids between the retryable write record and the transaction record.
    if (retryableWriteRecord.getSessionId().toBSON().woCompare(txnRecord.getSessionId().toBSON()) <
        0) {
        checkTxnEntry();
        checkRetryableWriteEntry();
    } else {
        checkRetryableWriteEntry();
        checkTxnEntry();
    }

    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyDeleteNotTouchingChunkIsIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kDelete,            // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdatePrePostNotTouchingChunkIsIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -5),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 1)),       // o
        BSON("x" << -5),                      // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        entry1.getOpTime());                  // pre-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageTouchingPostNotTouchingChunkShouldNotBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -50),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << -50)),     // o
        BSON("x" << 10),                      // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry1, entry2};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest,
       UpdatePreImageNotTouchingPostTouchingChunkShouldBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << 50),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("x" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, FindAndModifyUpdateNotTouchingChunkShouldBeIgnored) {
    auto entry1 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("x" << -10 << "y" << 50),        // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(52, 346), 2),  // optime
        repl::OpTypeEnum::kUpdate,            // op type
        BSON("$set" << BSON("y" << 50)),      // o
        BSON("x" << -10),                     // o2
        Date_t::now(),                        // wall clock time
        {1},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0),     // optime of previous write within same transaction
        boost::none,                          // pre-image optime
        entry1.getOpTime());                  // post-image optime
    insertOplogEntry(entry2);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry2.getOpTime());
    sessionRecord.setLastWriteDate(entry2.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
}

TEST_F(SessionCatalogMigrationSourceTest, TwoSessionWithTwoWritesContainingWriteNotInChunk) {
    auto sessionId1 = makeLogicalSessionIdForTest();
    auto sessionId2 = makeLogicalSessionIdForTest();

    auto cmpResult = sessionId1.toBSON().woCompare(sessionId2.toBSON());
    auto lowerSessionId = (cmpResult < 0) ? sessionId1 : sessionId2;
    auto higherSessionId = (cmpResult < 0) ? sessionId2 : sessionId1;

    auto entry1a = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    auto entry1b =
        makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                       repl::OpTypeEnum::kInsert,              // op type
                       BSON("x" << -50),                       // o
                       boost::none,                            // o2
                       Date_t::now(),                          // wall clock time
                       {1},                                    // statement ids
                       entry1a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord1;
    sessionRecord1.setSessionId(higherSessionId);
    sessionRecord1.setTxnNum(1);
    sessionRecord1.setLastWriteOpTime(entry1b.getOpTime());
    sessionRecord1.setLastWriteDate(entry1b.getWallClockTime());

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    auto entry2a = makeOplogEntry(
        repl::OpTime(Timestamp(43, 12), 2),  // optime
        repl::OpTypeEnum::kDelete,           // op type
        BSON("x" << 30),                     // o
        boost::none,                         // o2
        Date_t::now(),                       // wall clock time
        {3},                                 // statement ids
        repl::OpTime(Timestamp(0, 0), 0));   // optime of previous write within same transaction

    auto entry2b =
        makeOplogEntry(repl::OpTime(Timestamp(789, 13), 2),  // optime
                       repl::OpTypeEnum::kDelete,            // op type
                       BSON("x" << 50),                      // o
                       boost::none,                          // o2
                       Date_t::now(),                        // wall clock time
                       {4},                                  // statement ids
                       entry2a.getOpTime());  // optime of previous write within same transaction

    SessionTxnRecord sessionRecord2;
    sessionRecord2.setSessionId(lowerSessionId);
    sessionRecord2.setTxnNum(1);
    sessionRecord2.setLastWriteOpTime(entry2b.getOpTime());
    sessionRecord2.setLastWriteDate(entry2b.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord2.toBSON());

    insertOplogEntry(entry2a);
    insertOplogEntry(entry1a);
    insertOplogEntry(entry1b);
    insertOplogEntry(entry2b);

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));

    auto expectedSequence = {entry1a, entry2b, entry2a};

    for (auto oplog : expectedSequence) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());
        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        ASSERT_FALSE(nextOplogResult.shouldWaitForMajority);
        // Cannot compare directly because of SERVER-31356
        ASSERT_BSONOBJ_EQ(oplog.getEntry().toBSON(), nextOplogResult.oplog->getEntry().toBSON());
        migrationSource.fetchNextOplog(opCtx());
    }

    ASSERT_FALSE(migrationSource.hasMoreOplog());
}

TEST_F(SessionCatalogMigrationSourceTest, UntransferredDataSizeWithCommittedWrites) {
    DBDirectClient client(opCtx());
    client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
    // Enter an oplog entry before creating SessionCatalogMigrationSource to set config.transactions
    // average object size to the size of this entry.
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 0),                       // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry);

    SessionTxnRecord sessionRecord;
    sessionRecord.setSessionId(makeLogicalSessionIdForTest());
    sessionRecord.setTxnNum(1);
    sessionRecord.setLastWriteOpTime(entry.getOpTime());
    sessionRecord.setLastWriteDate(entry.getWallClockTime());

    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(), sessionRecord.toBSON());

    // Check for the initial state of the SessionCatalogMigrationSource, and drain the majority
    // committed session writes.
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_FALSE(migrationSource.inCatchupPhase());
    migrationSource.fetchNextOplog(opCtx());
    migrationSource.getLastFetchedOplog();
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.hasMoreOplog());

    // Test inCatchupPhase() and untransferredCatchUpDataSize() with new writes.
    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), sessionRecord.toBSON().objsize());

    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), 2 * sessionRecord.toBSON().objsize());

    // Drain new writes and check untransferred data size.
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_TRUE(migrationSource.fetchNextOplog(opCtx()));
    ASSERT_FALSE(migrationSource.fetchNextOplog(opCtx()));

    ASSERT_FALSE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    ASSERT_EQ(0, migrationSource.untransferredCatchUpDataSize());
}

TEST_F(SessionCatalogMigrationSourceTest, UntransferredDataSizeWithNoCommittedWrites) {
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 0),                       // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction
    insertOplogEntry(entry);
    migrationSource.notifyNewWriteOpTime(
        entry.getOpTime(), SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);

    ASSERT_TRUE(migrationSource.hasMoreOplog());
    ASSERT_TRUE(migrationSource.inCatchupPhase());
    // Average object size is default since the config.transactions collection does not exist.
    const int64_t defaultSessionDocSize =
        sizeof(LogicalSessionId) + sizeof(TxnNumber) + sizeof(Timestamp) + 16;
    ASSERT_EQ(migrationSource.untransferredCatchUpDataSize(), defaultSessionDocSize);
}

TEST_F(SessionCatalogMigrationSourceTest, FilterRewrittenOplogEntriesOutsideChunkRange) {

    auto data = {std::make_pair(BSON("x" << 30), repl::OpTime(Timestamp(52, 345), 2)),
                 std::make_pair(BSON("x" << -50), repl::OpTime(Timestamp(67, 54801), 2)),
                 std::make_pair(BSON("x" << 40), repl::OpTime(Timestamp(43, 12), 2)),
                 std::make_pair(BSON("x" << 50), repl::OpTime(Timestamp(789, 13), 2))};

    std::vector<repl::OplogEntry> entries;
    std::transform(data.begin(), data.end(), std::back_inserter(entries), [](const auto& pair) {
        auto original =
            makeOplogEntry(pair.second,                // optime
                           repl::OpTypeEnum::kInsert,  // op type
                           pair.first,                 // o
                           boost::none,                // o2
                           Date_t::now(),              // wall clock time
                           {0},                        // statement ids
                           repl::OpTime(Timestamp(0, 0),
                                        0));  // optime of previous write within same transaction
        return makeOplogEntry(pair.second,    // optime
                              repl::OpTypeEnum::kNoop,  // op type
                              BSON(SessionCatalogMigration::kSessionMigrateOplogTag << 1),  // o
                              original.getEntry().toBSON(),                                 // o2
                              original.getWallClockTime(),  // wall clock time
                              {0},                          // statement ids
                              repl::OpTime(Timestamp(0, 0),
                                           0));  // optime of previous write within same transaction
    });


    DBDirectClient client(opCtx());
    for (auto entry : entries) {
        SessionTxnRecord sessionRecord(
            makeLogicalSessionIdForTest(), 1, entry.getOpTime(), entry.getWallClockTime());

        client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                      sessionRecord.toBSON());
        insertOplogEntry(entry);
    }
    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);
    std::vector<repl::OplogEntry> filteredEntries = {entries.at(1)};

    while (migrationSource.fetchNextOplog(opCtx())) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();
        std::for_each(
            filteredEntries.begin(), filteredEntries.end(), [nextOplogResult](auto& entry) {
                ASSERT_BSONOBJ_NE(entry.getEntry().toBSON(),
                                  nextOplogResult.oplog->getEntry().toBSON());
            });
    }
}

TEST_F(SessionCatalogMigrationSourceTest,
       FilterSingleSessionRewrittenOplogEntriesOutsideChunkRange) {

    auto rewrittenEntryOne = makeRewrittenOplogInSession(
        repl::OpTime(Timestamp(52, 345), 2), repl::OpTime(Timestamp(0, 0), 0), BSON("x" << 30), 0);

    auto rewrittenEntryTwo = makeRewrittenOplogInSession(
        repl::OpTime(Timestamp(67, 54801), 2), rewrittenEntryOne.getOpTime(), BSON("x" << -50), 1);

    std::vector<repl::OplogEntry> entries = {rewrittenEntryOne, rewrittenEntryTwo};

    SessionTxnRecord sessionRecord1(makeLogicalSessionIdForTest(),
                                    1,
                                    rewrittenEntryTwo.getOpTime(),
                                    rewrittenEntryTwo.getWallClockTime());
    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                  sessionRecord1.toBSON());

    for (auto entry : entries) {
        insertOplogEntry(entry);
    }

    SessionCatalogMigrationSource migrationSource(opCtx(), kNs, kChunkRange, kShardKey);

    std::vector<repl::OplogEntry> filteredEntries = {entries.at(1)};

    while (migrationSource.fetchNextOplog(opCtx())) {
        ASSERT_TRUE(migrationSource.hasMoreOplog());

        auto nextOplogResult = migrationSource.getLastFetchedOplog();

        std::for_each(
            filteredEntries.begin(), filteredEntries.end(), [nextOplogResult](auto& entry) {
                ASSERT_BSONOBJ_NE(entry.getEntry().toBSON(),
                                  nextOplogResult.oplog->getEntry().toBSON());
            });
    }
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsTrueForCrudOplogEntryOutsideChunkRange) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    auto skippedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << -30),                     // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    ASSERT_TRUE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        skippedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForCrudOplogEntryInChunkRange) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << 30),                      // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForUserDocumentWithSessionMigrateOplogTag) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);

    // This oplogEntry represents the preImage document stored in a no-op oplogEntry.
    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kNoop,              // op type
        BSON("_id" << 5 << "x" << 30 << SessionCatalogMigration::kSessionMigrateOplogTag
                   << 1),                   // o
        BSONObj(),                          // o2
        Date_t::now(),                      // wall clock time
        {0},                                // statement ids
        repl::OpTime(Timestamp(0, 0), 0));  // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsFalseForRewrittenOplogInChunkRange) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    BSONObj emptyObject;

    auto rewrittenEntryOne = makeRewrittenOplogInSession(
        repl::OpTime(Timestamp(52, 345), 2), repl::OpTime(Timestamp(0, 0), 0), BSON("x" << 30), 0);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest,
       ShouldSkipOplogEntryReturnsTrueForRewrittenOplogInChunkRange) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    BSONObj emptyObject;

    auto rewrittenEntryOne = makeRewrittenOplogInSession(
        repl::OpTime(Timestamp(52, 345), 2), repl::OpTime(Timestamp(0, 0), 0), BSON("x" << -30), 0);

    ASSERT_TRUE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryReturnsFalseForDeadSentinel) {
    const auto shardKeyPattern = ShardKeyPattern(kShardKey);
    auto wallClockTime = Date_t::now();
    auto deadSentinel = makeSentinelOplogEntry(makeLogicalSessionIdForTest(), 1, wallClockTime);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        deadSentinel, shardKeyPattern, kChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryWorksWithNestedShardKeys) {
    const auto shardKeyPattern = ShardKeyPattern(kNestedShardKey);

    auto processedEntry = makeOplogEntry(
        repl::OpTime(Timestamp(52, 345), 2),  // optime
        repl::OpTypeEnum::kInsert,            // op type
        BSON("x" << BSON("y" << 30)),         // o
        boost::none,                          // o2
        Date_t::now(),                        // wall clock time
        {0},                                  // statement ids
        repl::OpTime(Timestamp(0, 0), 0));    // optime of previous write within same transaction

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        processedEntry, shardKeyPattern, kNestedChunkRange));
}

TEST_F(SessionCatalogMigrationSourceTest, ShouldSkipOplogEntryWorksWithRewrittenNestedShardKeys) {
    const auto shardKeyPattern = ShardKeyPattern(kNestedShardKey);

    auto rewrittenEntryOne = makeRewrittenOplogInSession(repl::OpTime(Timestamp(52, 345), 2),
                                                         repl::OpTime(Timestamp(0, 0), 0),
                                                         BSON("x" << BSON("y" << 30)),
                                                         0);

    ASSERT_FALSE(SessionCatalogMigrationSource::shouldSkipOplogEntry(
        rewrittenEntryOne, shardKeyPattern, kNestedChunkRange));
}

}  // namespace
}  // namespace mongo

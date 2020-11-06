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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding_txn_cloner.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingTxnClonerTest : public ShardServerTestFixture {
    void setUp() {
        ShardServerTestFixture::setUp();
        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
    }

    void tearDown() {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds)
                : ShardingCatalogClientMock(nullptr), _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kTwoShardIdList);
    }

protected:
    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};

    const std::vector<DurableTxnStateEnum> kDurableTxnStateEnumValues = {
        DurableTxnStateEnum::kPrepared,
        DurableTxnStateEnum::kCommitted,
        DurableTxnStateEnum::kAborted,
        DurableTxnStateEnum::kInProgress};

    std::vector<boost::optional<DurableTxnStateEnum>> getDurableTxnStatesAndBoostNone() {
        std::vector<boost::optional<DurableTxnStateEnum>> statesAndBoostNone = {boost::none};
        for (auto state : kDurableTxnStateEnumValues) {
            statesAndBoostNone.push_back(state);
        }
        return statesAndBoostNone;
    }

    BSONObj makeTxn(boost::optional<DurableTxnStateEnum> multiDocTxnState = boost::none) {
        auto txn = SessionTxnRecord(
            makeLogicalSessionIdForTest(), 0, repl::OpTime(Timestamp::min(), 0), Date_t());
        txn.setState(multiDocTxnState);
        return txn.toBSON();
    }

    void onCommandReturnTxns(std::vector<BSONObj> firstBatch, std::vector<BSONObj> secondBatch) {
        CursorId cursorId(0);
        if (secondBatch.size() > 0) {
            cursorId = 123;
        }
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return CursorResponse(
                       NamespaceString::kSessionTransactionsTableNamespace, cursorId, firstBatch)
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });

        if (secondBatch.size() == 0) {
            return;
        }

        onCommand([&](const executor::RemoteCommandRequest& request) {
            return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                                  CursorId{0},
                                  secondBatch)
                .toBSON(CursorResponse::ResponseType::SubsequentResponse);
        });
    }

    void seedTransactionOnRecipient(LogicalSessionId sessionId,
                                    TxnNumber txnNum,
                                    bool multiDocTxn) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);

        if (multiDocTxn) {
            opCtx->setInMultiDocumentTransaction();
        }

        MongoDOperationContextSession ocs(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT(txnParticipant);
        if (multiDocTxn) {
            txnParticipant.beginOrContinue(opCtx, txnNum, false, true);
        } else {
            txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);
        }
    }

    void checkTxnHasBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        DBDirectClient client(operationContext());
        auto bsonOplog =
            client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                           BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));
        ASSERT(!bsonOplog.isEmpty());
        auto oplogEntry = repl::MutableOplogEntry::parse(bsonOplog).getValue();
        ASSERT_EQ(oplogEntry.getTxnNumber().get(), txnNum);
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject(), BSON("$sessionMigrateInfo" << 1));
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject2().get(), BSON("$incompleteOplogHistory" << 1));
        ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kNoop);

        auto bsonTxn =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                           {BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON())});
        ASSERT(!bsonTxn.isEmpty());
        auto txn = SessionTxnRecord::parse(
            IDLParserErrorContext("resharding config transactions cloning test"), bsonTxn);
        ASSERT_EQ(txn.getTxnNum(), txnNum);
        ASSERT_EQ(txn.getLastWriteOpTime(), oplogEntry.getOpTime());
    }
    void checkTxnHasNotBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        DBDirectClient client(operationContext());
        auto bsonOplog =
            client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                           BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));

        ASSERT_BSONOBJ_EQ(bsonOplog, {});
        ASSERT_EQ(txnParticipant.getActiveTxnNumber(), txnNum);
    }

private:
    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }
};

TEST_F(ReshardingTxnClonerTest, TxnAggregation) {
    std::vector<BSONObj> expectedTransactions{makeTxn(),
                                              makeTxn(),
                                              makeTxn(DurableTxnStateEnum::kPrepared),
                                              makeTxn(DurableTxnStateEnum::kCommitted),
                                              makeTxn(DurableTxnStateEnum::kAborted),
                                              makeTxn(DurableTxnStateEnum::kInProgress),
                                              makeTxn()};
    std::vector<BSONObj> retrievedTransactions;

    auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                kTwoShardIdList[1],
                                                Timestamp::max(),
                                                boost::none,
                                                [&](OperationContext* opCtx, BSONObj transaction) {
                                                    retrievedTransactions.push_back(transaction);
                                                },
                                                nullptr);

    onCommandReturnTxns(
        std::vector<BSONObj>(expectedTransactions.begin(), expectedTransactions.begin() + 4),
        std::vector<BSONObj>(expectedTransactions.begin() + 4, expectedTransactions.end()));

    fetcher->join();

    ASSERT(std::equal(expectedTransactions.begin(),
                      expectedTransactions.end(),
                      retrievedTransactions.begin(),
                      [](BSONObj a, BSONObj b) { return a.binaryEqual(b); }));
}

TEST_F(ReshardingTxnClonerTest, CursorNotFoundError) {
    std::vector<BSONObj> expectedTransactions{makeTxn(),
                                              makeTxn(),
                                              makeTxn(DurableTxnStateEnum::kPrepared),
                                              makeTxn(DurableTxnStateEnum::kCommitted),
                                              makeTxn(DurableTxnStateEnum::kAborted),
                                              makeTxn(DurableTxnStateEnum::kInProgress),
                                              makeTxn()};
    std::vector<BSONObj> retrievedTransactions;
    Status error = Status::OK();

    auto fetcher = cloneConfigTxnsForResharding(
        operationContext(),
        kTwoShardIdList[1],
        Timestamp::max(),
        boost::none,
        [&](auto opCtx, BSONObj transaction) { retrievedTransactions.push_back(transaction); },
        &error);

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{123},
                              expectedTransactions)
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::CursorNotFound, "Simulate cursor not found error");
    });

    fetcher->join();

    ASSERT(std::equal(expectedTransactions.begin(),
                      expectedTransactions.end(),
                      retrievedTransactions.begin(),
                      [](BSONObj a, BSONObj b) { return a.binaryEqual(b); }));
    ASSERT_EQ(error, ErrorCodes::CursorNotFound);
}

TEST_F(ReshardingTxnClonerTest, MergeTxnNotOnRecipient) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasBeenUpdated(sessionId, txnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeUnParsableTxn) {
    Status status = Status::OK();

    auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                kTwoShardIdList[1],
                                                Timestamp::max(),
                                                boost::none,
                                                &configTxnsMergerForResharding,
                                                &status);

    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 3;
    onCommandReturnTxns({SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now())
                             .toBSON()
                             .removeField(SessionTxnRecord::kSessionIdFieldName)},
                        {});

    fetcher->join();

    ASSERT_EQ(status.code(), 40414);
}

TEST_F(ReshardingTxnClonerTest, MergeNewTxnOverMultiDocTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber donorTxnNum = 3;
        TxnNumber recipientTxnNum = donorTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, true);

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasBeenUpdated(sessionId, donorTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeNewTxnOverRetryableWriteTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber donorTxnNum = 3;
        TxnNumber recipientTxnNum = donorTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, false);

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasBeenUpdated(sessionId, donorTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeCurrentTxnOverRetryableWriteTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        seedTransactionOnRecipient(sessionId, txnNum, false);

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasBeenUpdated(sessionId, txnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeCurrentTxnOverMultiDocTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        seedTransactionOnRecipient(sessionId, txnNum, true);

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasNotBeenUpdated(sessionId, txnNum);
    }
}


TEST_F(ReshardingTxnClonerTest, MergeOldTxnOverTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        Status status = Status::OK();

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber recipientTxnNum = 3;
        TxnNumber donorTxnNum = recipientTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, false);

        auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                    kTwoShardIdList[1],
                                                    Timestamp::max(),
                                                    boost::none,
                                                    &configTxnsMergerForResharding,
                                                    &status);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        fetcher->join();

        ASSERT(status.isOK()) << str::stream()
                              << (state ? DurableTxnState_serializer(*state) : "retryable write");
        checkTxnHasNotBeenUpdated(sessionId, recipientTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeMultiDocTransactionAndRetryableWrite) {
    Status status = Status::OK();

    const auto sessionIdRetryableWrite = makeLogicalSessionIdForTest();
    const auto sessionIdMultiDocTxn = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 3;

    auto fetcher = cloneConfigTxnsForResharding(operationContext(),
                                                kTwoShardIdList[1],
                                                Timestamp::max(),
                                                boost::none,
                                                &configTxnsMergerForResharding,
                                                &status);

    auto sessionRecordRetryableWrite =
        SessionTxnRecord(sessionIdRetryableWrite, txnNum, repl::OpTime(), Date_t::now());
    auto sessionRecordMultiDocTxn =
        SessionTxnRecord(sessionIdMultiDocTxn, txnNum, repl::OpTime(), Date_t::now());
    sessionRecordMultiDocTxn.setState(DurableTxnStateEnum::kAborted);

    onCommandReturnTxns({sessionRecordRetryableWrite.toBSON(), sessionRecordMultiDocTxn.toBSON()},
                        {});

    fetcher->join();

    ASSERT(status.isOK());
    checkTxnHasBeenUpdated(sessionIdRetryableWrite, txnNum);
    checkTxnHasBeenUpdated(sessionIdMultiDocTxn, txnNum);
}

}  // namespace
}  // namespace mongo

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

#include "mongo/db/transaction_reaper.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/db/transaction_reaper_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

/**
 * Makes the query we'll use to scan the transactions table.
 *
 * Scans for records older than the minimum lifetime and uses a sort to walk the index and attempt
 * to pull records likely to be on the same chunks (because they sort near each other).
 */
Query makeQuery(Date_t now) {
    const Date_t possiblyExpired(now - Minutes(gTransactionRecordMinimumLifetimeMinutes));
    Query query(BSON(kLastWriteDateFieldName << LT << possiblyExpired));
    query.sort(kSortById);
    return query;
}

/**
 * Our impl is templatized on a type which handles the lsids we see.  It provides the top level
 * scaffolding for figuring out if we're the primary node responsible for the transaction table and
 * invoking the handler.
 *
 * The handler here will see all of the possibly expired txn ids in the transaction table and will
 * have a lifetime associated with a single call to reap.
 */
template <typename Handler>
class TransactionReaperImpl final : public TransactionReaper {
public:
    TransactionReaperImpl(std::shared_ptr<SessionsCollection> collection)
        : _collection(std::move(collection)) {}

    int reap(OperationContext* opCtx) override {
        Handler handler(opCtx, *_collection);
        if (!handler.initialize()) {
            return 0;
        }

        // Make a best-effort attempt to only reap when the node is running as a primary
        const auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
        if (!coord->canAcceptWritesForDatabase_UNSAFE(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db())) {
            return 0;
        }

        DBDirectClient client(opCtx);

        // Fill all stale config.transactions entries
        auto query = makeQuery(opCtx->getServiceContext()->getFastClockSource()->now());
        auto cursor = client.query(
            NamespaceString::kSessionTransactionsTableNamespace, query, 0, 0, &kIdProjection);

        while (cursor->more()) {
            auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
                "TransactionSession"_sd, cursor->next());

            handler.handleLsid(transactionSession.get_id());
        }

        // Before the handler goes out of scope, flush its last batch to disk and collect stats.
        return handler.finalize();
    }

private:
    std::shared_ptr<SessionsCollection> _collection;
};

/**
 * Removes the specified set of session ids from the persistent sessions collection and returns the
 * number of sessions actually removed.
 */
int removeSessionsTransactionRecords(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     const LogicalSessionIdSet& sessionIdsToRemove) {
    if (sessionIdsToRemove.empty()) {
        return 0;
    }

    // From the passed-in sessions, find the ones which are actually expired/removed
    auto expiredSessionIds =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, sessionIdsToRemove));

    DBDirectClient client(opCtx);
    int numDeleted = 0;

    for (auto it = expiredSessionIds.begin(); it != expiredSessionIds.end();) {
        write_ops::Delete deleteOp(NamespaceString::kSessionTransactionsTableNamespace);
        deleteOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase base;
            base.setOrdered(false);
            return base;
        }());
        deleteOp.setDeletes([&] {
            // The max batch size is chosen so that a single batch won't exceed the 16MB BSON object
            // size limit
            const int kMaxBatchSize = 10'000;
            std::vector<write_ops::DeleteOpEntry> entries;
            for (; it != expiredSessionIds.end() && entries.size() < kMaxBatchSize; ++it) {
                entries.emplace_back(BSON(LogicalSessionRecord::kIdFieldName << it->toBSON()),
                                     false /* multi = false */);
            }
            return entries;
        }());

        BSONObj result;
        client.runCommand(NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
                          deleteOp.toBSON({}),
                          result);

        BatchedCommandResponse response;
        std::string errmsg;
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Failed to parse response " << result,
                response.parseBSON(result, &errmsg));
        uassertStatusOK(response.getTopLevelStatus());

        numDeleted += response.getN();
    }

    return numDeleted;
}

/**
 * The repl impl is simple, just pass along to the sessions collection for checking ids locally
 */
class ReplHandler {
public:
    ReplHandler(OperationContext* opCtx, SessionsCollection& sessionsCollection)
        : _opCtx(opCtx), _sessionsCollection(sessionsCollection) {}

    bool initialize() {
        return true;
    }

    void handleLsid(const LogicalSessionId& lsid) {
        _batch.insert(lsid);

        if (_batch.size() >= write_ops::kMaxWriteBatchSize) {
            _numReaped += removeSessionsTransactionRecords(_opCtx, _sessionsCollection, _batch);
            _batch.clear();
        }
    }

    int finalize() {
        invariant(!_finalized);
        _finalized = true;

        _numReaped += removeSessionsTransactionRecords(_opCtx, _sessionsCollection, _batch);
        return _numReaped;
    }

private:
    OperationContext* const _opCtx;
    SessionsCollection& _sessionsCollection;

    LogicalSessionIdSet _batch;

    int _numReaped{0};

    bool _finalized{false};
};

/**
 * The sharding impl is a little fancier.  Try to bucket by shard id, to avoid doing repeated small
 * scans.
 */
class ShardedHandler {
public:
    ShardedHandler(OperationContext* opCtx, SessionsCollection& sessionsCollection)
        : _opCtx(opCtx), _sessionsCollection(sessionsCollection) {}

    // Returns false if the sessions collection is not set up.
    bool initialize() {
        auto routingInfo =
            uassertStatusOK(Grid::get(_opCtx)->catalogCache()->getCollectionRoutingInfo(
                _opCtx, NamespaceString::kLogicalSessionsNamespace));
        _cm = routingInfo.cm();
        return !!_cm;
    }

    void handleLsid(const LogicalSessionId& lsid) {
        invariant(_cm);

        // This code attempts to group requests to 'removeSessionsTransactionRecords' to contain
        // batches of lsids, which only fall on the same shard, so that the query to check whether
        // they are alive doesn't need to do cross-shard scatter/gather queries
        const auto chunk = _cm->findIntersectingChunkWithSimpleCollation(lsid.toBSON());
        const auto& shardId = chunk.getShardId();

        auto& lsids = _shards[shardId];
        lsids.insert(lsid);

        if (lsids.size() >= write_ops::kMaxWriteBatchSize) {
            _numReaped += removeSessionsTransactionRecords(_opCtx, _sessionsCollection, lsids);
            _shards.erase(shardId);
        }
    }

    int finalize() {
        invariant(!_finalized);
        _finalized = true;

        for (const auto& pair : _shards) {
            _numReaped +=
                removeSessionsTransactionRecords(_opCtx, _sessionsCollection, pair.second);
        }

        return _numReaped;
    }

private:
    OperationContext* const _opCtx;
    SessionsCollection& _sessionsCollection;

    std::shared_ptr<ChunkManager> _cm;

    stdx::unordered_map<ShardId, LogicalSessionIdSet, ShardId::Hasher> _shards;
    int _numReaped{0};

    bool _finalized{false};
};

}  // namespace

std::unique_ptr<TransactionReaper> TransactionReaper::make(
    Type type, std::shared_ptr<SessionsCollection> collection) {
    switch (type) {
        case Type::kReplicaSet:
            return stdx::make_unique<TransactionReaperImpl<ReplHandler>>(std::move(collection));
        case Type::kSharded:
            return stdx::make_unique<TransactionReaperImpl<ShardedHandler>>(std::move(collection));
    }
    MONGO_UNREACHABLE;
}

TransactionReaper::~TransactionReaper() = default;

}  // namespace mongo

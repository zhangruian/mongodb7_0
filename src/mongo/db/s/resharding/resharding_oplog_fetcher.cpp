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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace {
boost::intrusive_ptr<ExpressionContext> _makeExpressionContext(OperationContext* opCtx) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    const NamespaceString slimOplogNs("local.system.resharding.slimOplogForGraphLookup");
    resolvedNamespaces[slimOplogNs.coll()] = {slimOplogNs, std::vector<BSONObj>()};
    resolvedNamespaces[NamespaceString::kRsOplogNamespace.coll()] = {
        NamespaceString::kRsOplogNamespace, std::vector<BSONObj>()};
    return make_intrusive<ExpressionContext>(opCtx,
                                             boost::none, /* explain */
                                             false,       /* fromMongos */
                                             false,       /* needsMerge */
                                             true,        /* allowDiskUse */
                                             true,        /* bypassDocumentValidation */
                                             false,       /* isMapReduceCommand */
                                             NamespaceString::kRsOplogNamespace,
                                             boost::none, /* runtimeConstants */
                                             nullptr,     /* collator */
                                             MongoProcessInterface::create(opCtx),
                                             std::move(resolvedNamespaces),
                                             boost::none); /* collUUID */
}
}  // namespace

ReshardingOplogFetcher::ReshardingOplogFetcher(std::unique_ptr<Env> env,
                                               UUID reshardingUUID,
                                               UUID collUUID,
                                               ReshardingDonorOplogId startAt,
                                               ShardId donorShard,
                                               ShardId recipientShard,
                                               NamespaceString toWriteInto)
    : _env(std::move(env)),
      _reshardingUUID(reshardingUUID),
      _collUUID(collUUID),
      _startAt(startAt),
      _donorShard(donorShard),
      _recipientShard(recipientShard),
      _toWriteInto(toWriteInto) {
    auto [p, f] = makePromiseFuture<void>();
    stdx::lock_guard lk(_mutex);
    _onInsertPromise = std::move(p);
    _onInsertFuture = std::move(f);
}

ReshardingOplogFetcher::~ReshardingOplogFetcher() {
    stdx::lock_guard lk(_mutex);
    _onInsertPromise.setError(
        {ErrorCodes::CallbackCanceled, "explicitly breaking promise from ReshardingOplogFetcher"});
}

Future<void> ReshardingOplogFetcher::awaitInsert(const ReshardingDonorOplogId& lastSeen) {
    // `lastSeen` is the _id of the document ReshardingDonorOplogIterator::getNextBatch() has last
    // read from the oplog buffer collection.
    //
    // `_startAt` is updated after each insert into the oplog buffer collection by
    // ReshardingOplogFetcher to reflect the newer resume point if a new aggregation request was
    // being issued.

    stdx::lock_guard lk(_mutex);
    if (lastSeen < _startAt) {
        // `lastSeen < _startAt` means there's at least one document which has been inserted by
        // ReshardingOplogFetcher and hasn't been returned by
        // ReshardingDonorOplogIterator::getNextBatch(). The caller has no reason to wait until yet
        // another document has been inserted before reading from the oplog buffer collection.
        return Future<void>::makeReady();
    }

    // `lastSeen == _startAt` means the last document inserted by ReshardingOplogFetcher has already
    // been returned by ReshardingDonorOplogIterator::getNextBatch() and so
    // ReshardingDonorOplogIterator would want to wait until ReshardingOplogFetcher does another
    // insert.
    //
    // `lastSeen > _startAt` isn't expected to happen in practice because
    // ReshardingDonorOplogIterator only uses _id's from documents that it actually read from the
    // oplog buffer collection for `lastSeen`, but would also mean the caller wants to wait.
    return std::move(_onInsertFuture);
}

ExecutorFuture<void> ReshardingOplogFetcher::schedule(
    std::shared_ptr<executor::TaskExecutor> executor, const CancellationToken& cancelToken) {
    return ExecutorFuture(executor)
        .then(
            [this, executor, cancelToken] { return _reschedule(std::move(executor), cancelToken); })
        .onError([](Status status) {
            LOGV2_INFO(5192101, "Resharding oplog fetcher aborting", "reason"_attr = status);
            return status;
        });
}

ExecutorFuture<void> ReshardingOplogFetcher::_reschedule(
    std::shared_ptr<executor::TaskExecutor> executor, const CancellationToken& cancelToken) {
    return ExecutorFuture(executor)
        .then([this, executor, cancelToken] {
            ThreadClient client(fmt::format("OplogFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                _service());
            return iterate(client.get());
        })
        .then([executor, cancelToken](bool moreToCome) {
            // Wait a little before re-running the aggregation pipeline on the donor's oplog. The
            // 1-second value was chosen to match the default awaitData timeout that would have been
            // used if the aggregation cursor was TailableModeEnum::kTailableAndAwaitData.
            return executor->sleepFor(Seconds{1}, cancelToken).then([moreToCome] {
                return moreToCome;
            });
        })
        .then([this, executor, cancelToken](bool moreToCome) {
            if (!moreToCome) {
                return ExecutorFuture(std::move(executor));
            }

            if (cancelToken.isCanceled()) {
                return ExecutorFuture<void>(
                    executor,
                    Status{ErrorCodes::CallbackCanceled,
                           "Resharding oplog fetcher canceled due to abort or stepdown"});
            }
            return _reschedule(std::move(executor), cancelToken);
        });
}

bool ReshardingOplogFetcher::iterate(Client* client) {
    std::shared_ptr<Shard> targetShard;
    {
        auto opCtxRaii = client->makeOperationContext();
        opCtxRaii->checkForInterrupt();

        StatusWith<std::shared_ptr<Shard>> swDonor =
            Grid::get(opCtxRaii.get())->shardRegistry()->getShard(opCtxRaii.get(), _donorShard);
        if (!swDonor.isOK()) {
            LOGV2_WARNING(5127203,
                          "Error finding shard in registry, retrying.",
                          "error"_attr = swDonor.getStatus());
            return true;
        }
        targetShard = swDonor.getValue();
    }

    try {
        return consume(client, targetShard.get());
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        return false;
    } catch (const ExceptionFor<ErrorCodes::OplogQueryMinTsMissing>&) {
        LOGV2_ERROR(
            5192103, "Fatal resharding error while fetching.", "error"_attr = exceptionToStatus());
        throw;
    } catch (const DBException&) {
        LOGV2_WARNING(
            5127200, "Error while fetching, retrying.", "error"_attr = exceptionToStatus());
        return true;
    }
}

void ReshardingOplogFetcher::_ensureCollection(Client* client, const NamespaceString nss) {
    auto opCtxRaii = client->makeOperationContext();
    auto opCtx = opCtxRaii.get();
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // Create the destination collection if necessary.
    writeConflictRetry(opCtx, "createReshardingLocalOplogBuffer", nss.toString(), [&] {
        const CollectionPtr coll =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
        if (coll) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        AutoGetOrCreateDb db(opCtx, nss.db(), LockMode::MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        db.getDb()->createCollection(opCtx, nss);
        wuow.commit();
    });
}

AggregateCommand ReshardingOplogFetcher::_makeAggregateCommand(Client* client) {
    auto opCtxRaii = client->makeOperationContext();
    auto opCtx = opCtxRaii.get();
    auto expCtx = _makeExpressionContext(opCtx);

    auto serializedPipeline =
        createOplogFetchingPipelineForResharding(expCtx, _startAt, _collUUID, _recipientShard)
            ->serializeToBson();

    AggregateCommand aggRequest(NamespaceString::kRsOplogNamespace, std::move(serializedPipeline));
    if (_useReadConcern) {
        auto readConcernArgs = repl::ReadConcernArgs(
            boost::optional<LogicalTime>(_startAt.getTs()),
            boost::optional<repl::ReadConcernLevel>(repl::ReadConcernLevel::kMajorityReadConcern));
        aggRequest.setReadConcern(readConcernArgs.toBSONInner());
    }

    ReadPreferenceSetting readPref(ReadPreference::Nearest,
                                   ReadPreferenceSetting::kMinimalMaxStalenessValue);
    aggRequest.setUnwrappedReadPref(readPref.toContainingBSON());

    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setHint(BSON("$natural" << 1));
    aggRequest.setRequestReshardingResumeToken(true);

    if (_initialBatchSize) {
        SimpleCursorOptions cursor;
        cursor.setBatchSize(_initialBatchSize);
        aggRequest.setCursor(cursor);
    }

    return aggRequest;
}

bool ReshardingOplogFetcher::consume(Client* client, Shard* shard) {
    _ensureCollection(client, _toWriteInto);

    auto aggRequest = _makeAggregateCommand(client);

    auto opCtxRaii = client->makeOperationContext();
    int batchesProcessed = 0;
    bool moreToCome = true;
    // Note that the oplog entries are *not* being copied with a tailable cursor.
    // Shard::runAggregation() will instead return upon hitting the end of the donor's oplog.
    uassertStatusOK(shard->runAggregation(
        opCtxRaii.get(),
        aggRequest,
        [this, &batchesProcessed, &moreToCome](const std::vector<BSONObj>& batch) {
            ThreadClient client(fmt::format("ReshardingFetcher-{}-{}",
                                            _reshardingUUID.toString(),
                                            _donorShard.toString()),
                                _service(),
                                nullptr);
            auto opCtxRaii = cc().makeOperationContext();
            auto opCtx = opCtxRaii.get();

            // Noting some possible optimizations:
            //
            // * Batch more inserts into larger storage transactions.
            // * Parallize writing documents across multiple threads.
            // * Doing either of the above while still using the underlying message buffer of bson
            //   objects.
            AutoGetCollection toWriteTo(opCtx, _toWriteInto, LockMode::MODE_IX);
            for (const BSONObj& doc : batch) {
                WriteUnitOfWork wuow(opCtx);
                auto nextOplog = uassertStatusOK(repl::OplogEntry::parse(doc));

                auto startAt = ReshardingDonorOplogId::parse(
                    {"OplogFetcherParsing"}, nextOplog.get_id()->getDocument().toBson());
                uassertStatusOK(toWriteTo->insertDocument(opCtx, InsertStatement{doc}, nullptr));
                wuow.commit();
                ++_numOplogEntriesCopied;

                _env->metrics()->onOplogEntriesFetched(1);

                auto [p, f] = makePromiseFuture<void>();
                {
                    stdx::lock_guard lk(_mutex);
                    _startAt = startAt;
                    _onInsertPromise.emplaceValue();
                    _onInsertPromise = std::move(p);
                    _onInsertFuture = std::move(f);
                }

                if (isFinalOplog(nextOplog, _reshardingUUID)) {
                    moreToCome = false;
                    return false;
                }
            }

            if (_maxBatches > -1 && ++batchesProcessed >= _maxBatches) {
                return false;
            }

            return true;
        }));

    return moreToCome;
}

}  // namespace mongo

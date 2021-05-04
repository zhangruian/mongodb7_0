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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_oplog_applier.h"

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

ReshardingOplogApplier::ReshardingOplogApplier(
    std::unique_ptr<Env> env,
    ReshardingSourceId sourceId,
    NamespaceString outputNss,
    std::vector<NamespaceString> allStashNss,
    size_t myStashIdx,
    ChunkManager sourceChunkMgr,
    std::unique_ptr<ReshardingDonorOplogIteratorInterface> oplogIterator)
    : _env(std::move(env)),
      _sourceId(std::move(sourceId)),
      _batchPreparer{CollatorInterface::cloneCollator(sourceChunkMgr.getDefaultCollator())},
      _crudApplication{std::move(outputNss),
                       std::move(allStashNss),
                       myStashIdx,
                       _sourceId.getShardId(),
                       std::move(sourceChunkMgr)},
      _sessionApplication{},
      _batchApplier{_crudApplication, _sessionApplication},
      _oplogIter(std::move(oplogIterator)) {}

SemiFuture<void> ReshardingOplogApplier::_applyBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory,
    bool isForSessionApplication) {
    auto currentWriterVectors = [&] {
        if (isForSessionApplication) {
            return _batchPreparer.makeSessionOpWriterVectors(_currentBatchToApply);
        } else {
            return _batchPreparer.makeCrudOpWriterVectors(_currentBatchToApply, _currentDerivedOps);
        }
    }();

    CancellationSource errorSource(cancelToken);

    std::vector<SharedSemiFuture<void>> batchApplierFutures;
    batchApplierFutures.reserve(currentWriterVectors.size());

    for (auto&& writer : currentWriterVectors) {
        if (!writer.empty()) {
            batchApplierFutures.emplace_back(
                _batchApplier.applyBatch(std::move(writer), executor, errorSource.token(), factory)
                    .share());
        }
    }

    return resharding::cancelWhenAnyErrorThenQuiesce(batchApplierFutures, executor, errorSource)
        .onError([](Status status) {
            LOGV2_ERROR(
                5012004, "Failed to apply operation in resharding", "error"_attr = redact(status));
            return status;
        })
        .semi();
}

SemiFuture<void> ReshardingOplogApplier::run(std::shared_ptr<executor::TaskExecutor> executor,
                                             CancellationToken cancelToken,
                                             CancelableOperationContextFactory factory) {
    return AsyncTry([this, executor, cancelToken, factory] {
               return _oplogIter->getNextBatch(executor, cancelToken, factory)
                   .thenRunOn(executor)
                   .then([this, executor, cancelToken, factory](OplogBatch batch) {
                       LOGV2_DEBUG(5391002, 3, "Starting batch", "batchSize"_attr = batch.size());
                       _currentBatchToApply = std::move(batch);

                       return _applyBatch(
                           executor, cancelToken, factory, false /* isForSessionApplication */);
                   })
                   .then([this, executor, cancelToken, factory] {
                       return _applyBatch(
                           executor, cancelToken, factory, true /* isForSessionApplication */);
                   })
                   .then([this, factory] {
                       if (_currentBatchToApply.empty()) {
                           // Increment the number of entries applied by 1 in order to account for
                           // the final oplog entry.
                           _env->metrics()->onOplogEntriesApplied(1);
                           return false;
                       }

                       auto opCtx = factory.makeOperationContext(&cc());
                       _clearAppliedOpsAndStoreProgress(opCtx.get());
                       return true;
                   });
           })
        .until([](const StatusWith<bool>& swMoreToApply) {
            return !swMoreToApply.isOK() || !swMoreToApply.getValue();
        })
        .on(executor, cancelToken)
        .ignoreValue()
        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .on() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        .onCompletion([](auto x) { return x; })
        .semi();
}

boost::optional<ReshardingOplogApplierProgress> ReshardingOplogApplier::checkStoredProgress(
    OperationContext* opCtx, const ReshardingSourceId& id) {
    DBDirectClient client(opCtx);
    auto doc = client.findOne(
        NamespaceString::kReshardingApplierProgressNamespace.ns(),
        BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << id.toBSON()));

    if (doc.isEmpty()) {
        return boost::none;
    }

    IDLParserErrorContext ctx("ReshardingOplogApplierProgress");
    return ReshardingOplogApplierProgress::parse(ctx, doc);
}

void ReshardingOplogApplier::_clearAppliedOpsAndStoreProgress(OperationContext* opCtx) {
    const auto& lastOplog = _currentBatchToApply.back();

    auto oplogId =
        ReshardingDonorOplogId::parse({"ReshardingOplogApplier::_clearAppliedOpsAndStoreProgress"},
                                      lastOplog.get_id()->getDocument().toBson());

    PersistentTaskStore<ReshardingOplogApplierProgress> store(
        NamespaceString::kReshardingApplierProgressNamespace);

    BSONObjBuilder builder;
    builder.append("$set",
                   BSON(ReshardingOplogApplierProgress::kProgressFieldName << oplogId.toBSON()));
    builder.append("$inc",
                   BSON(ReshardingOplogApplierProgress::kNumEntriesAppliedFieldName
                        << static_cast<long long>(_currentBatchToApply.size())));

    store.upsert(
        opCtx,
        QUERY(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << _sourceId.toBSON()),
        builder.obj());
    _env->metrics()->onOplogEntriesApplied(_currentBatchToApply.size());

    _currentBatchToApply.clear();
    _currentDerivedOps.clear();
}

NamespaceString ReshardingOplogApplier::ensureStashCollectionExists(
    OperationContext* opCtx,
    const UUID& existingUUID,
    const ShardId& donorShardId,
    const CollectionOptions& options) {
    auto nss = getLocalConflictStashNamespace(existingUUID, donorShardId);

    resharding::data_copy::ensureCollectionExists(opCtx, nss, options);
    return nss;
}

}  // namespace mongo

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include "mongo/db/repl/tenant_oplog_applier.h"

#include <algorithm>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/insert_group.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(hangInTenantOplogApplication);
MONGO_FAIL_POINT_DEFINE(fpBeforeTenantOplogApplyingBatch);

TenantOplogApplier::TenantOplogApplier(const UUID& migrationUuid,
                                       const std::string& tenantId,
                                       OpTime applyFromOpTime,
                                       RandomAccessOplogBuffer* oplogBuffer,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       ThreadPool* writerPool,
                                       const bool isResuming)
    : AbstractAsyncComponent(executor.get(), std::string("TenantOplogApplier_") + tenantId),
      _migrationUuid(migrationUuid),
      _tenantId(tenantId),
      _beginApplyingAfterOpTime(applyFromOpTime),
      _oplogBuffer(oplogBuffer),
      _executor(std::move(executor)),
      _writerPool(writerPool),
      _isResuming(isResuming) {}

TenantOplogApplier::~TenantOplogApplier() {
    shutdown();
    join();
}

SemiFuture<TenantOplogApplier::OpTimePair> TenantOplogApplier::getNotificationForOpTime(
    OpTime donorOpTime) {
    stdx::lock_guard lk(_mutex);
    // If we're not running, return a future with the status we shut down with.
    if (!_isActive_inlock()) {
        return SemiFuture<OpTimePair>::makeReady(_finalStatus);
    }
    // If this optime has already passed, just return a ready future.
    if (_lastAppliedOpTimesUpToLastBatch.donorOpTime >= donorOpTime ||
        _beginApplyingAfterOpTime >= donorOpTime) {
        return SemiFuture<OpTimePair>::makeReady(_lastAppliedOpTimesUpToLastBatch);
    }

    // This will pull a new future off the existing promise for this time if it exists, otherwise
    // it constructs a new promise and pulls a future off of it.
    auto [iter, isNew] = _opTimeNotificationList.try_emplace(donorOpTime);
    return iter->second.getFuture().semi();
}

OpTime TenantOplogApplier::getBeginApplyingOpTime_forTest() const {
    return _beginApplyingAfterOpTime;
}

Status TenantOplogApplier::_doStartup_inlock() noexcept {
    Timestamp resumeTs;
    if (_isResuming) {
        resumeTs = _beginApplyingAfterOpTime.getTimestamp();
    }
    _oplogBatcher =
        std::make_shared<TenantOplogBatcher>(_tenantId, _oplogBuffer, _executor, resumeTs);
    auto status = _oplogBatcher->startup();
    if (!status.isOK())
        return status;

    auto fut = _oplogBatcher->getNextBatch(
        TenantOplogBatcher::BatchLimits(std::size_t(tenantApplierBatchSizeBytes.load()),
                                        std::size_t(tenantApplierBatchSizeOps.load())));
    std::move(fut)
        .thenRunOn(_executor)
        .then([this, self = shared_from_this()](TenantOplogBatch batch) {
            _applyLoop(std::move(batch));
        })
        .onError([this, self = shared_from_this()](Status status) {
            invariant(_shouldStopApplying(status));
        })
        .getAsync([](auto status) {});
    return Status::OK();
}

void TenantOplogApplier::_setFinalStatusIfOk(WithLock, Status newStatus) {
    if (_finalStatus.isOK()) {
        _finalStatus = newStatus;
    }
}

void TenantOplogApplier::_doShutdown_inlock() noexcept {
    // Shutting down the oplog batcher will make the _applyLoop stop with an error future, thus
    // shutting down the applier.
    _oplogBatcher->shutdown();
    // Oplog applier executor can shutdown before executing _applyLoop() and shouldStopApplying().
    // This can cause the applier to miss notifying the waiters in _opTimeNotificationList. So,
    // shutdown() is responsible to notify those waiters when _applyLoop() is not running.
    if (!_applyLoopApplyingBatch) {
        // We actually hold the required lock, but the lock object itself is not passed through.
        _finishShutdown(WithLock::withoutLock(),
                        {ErrorCodes::CallbackCanceled, "Tenant oplog applier shut down"});
    }
}

void TenantOplogApplier::_preJoin() noexcept {
    if (_oplogBatcher) {
        _oplogBatcher->join();
    }
}

void TenantOplogApplier::_applyLoop(TenantOplogBatch batch) {
    {
        stdx::lock_guard lk(_mutex);
        // Applier is not active as someone might have called shutdown().
        if (!_isActive_inlock())
            return;
        _applyLoopApplyingBatch = true;
    }

    // Getting the future for the next batch here means the batcher can retrieve the next batch
    // while the applier is processing the current one.
    auto nextBatchFuture = _oplogBatcher->getNextBatch(
        TenantOplogBatcher::BatchLimits(std::size_t(tenantApplierBatchSizeBytes.load()),
                                        std::size_t(tenantApplierBatchSizeOps.load())));

    Status applyStatus{Status::OK()};
    try {
        _applyOplogBatch(&batch);
    } catch (const DBException& e) {
        applyStatus = e.toStatus();
    }

    if (_shouldStopApplying(applyStatus)) {
        return;
    }

    std::move(nextBatchFuture)
        .thenRunOn(_executor)
        .then([this, self = shared_from_this()](TenantOplogBatch batch) {
            _applyLoop(std::move(batch));
        })
        .onError([this, self = shared_from_this()](Status status) {
            invariant(_shouldStopApplying(status));
        })
        .getAsync([](auto status) {});
}

bool TenantOplogApplier::_shouldStopApplying(Status status) {
    {
        stdx::lock_guard lk(_mutex);
        _applyLoopApplyingBatch = false;

        if (!_isActive_inlock()) {
            return true;
        }

        if (_isShuttingDown_inlock()) {
            _finishShutdown(lk,
                            {ErrorCodes::CallbackCanceled, "Tenant oplog applier shutting down"});
            return true;
        }

        dassert(_finalStatus.isOK());
        // Set the _finalStatus. This guarantees that the shutdown() called after releasing
        // the mutex will signal donor opTime waiters with the 'status' error code and not with
        // ErrorCodes::CallbackCanceled.
        _setFinalStatusIfOk(lk, status);
        if (_finalStatus.isOK()) {
            return false;
        }
    }
    shutdown();
    return true;
}

void TenantOplogApplier::_finishShutdown(WithLock lk, Status status) {
    // shouldStopApplying() might have already set the final status. So, don't mask the original
    // error.
    _setFinalStatusIfOk(lk, status);
    LOGV2_DEBUG(4886005,
                1,
                "TenantOplogApplier::_finishShutdown",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "error"_attr = redact(_finalStatus));

    invariant(!_finalStatus.isOK());
    // Any unfulfilled notifications are errored out.
    for (auto& listEntry : _opTimeNotificationList) {
        listEntry.second.setError(_finalStatus);
    }
    _opTimeNotificationList.clear();
    _transitionToComplete_inlock();
}

void TenantOplogApplier::_applyOplogBatch(TenantOplogBatch* batch) {
    LOGV2_DEBUG(4886004,
                1,
                "Tenant Oplog Applier starting to apply batch",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "firstDonorOptime"_attr = batch->ops.front().entry.getOpTime(),
                "lastDonorOptime"_attr = batch->ops.back().entry.getOpTime());
    auto opCtx = cc().makeOperationContext();
    _checkNsAndUuidsBelongToTenant(opCtx.get(), *batch);
    auto writerVectors = _fillWriterVectors(opCtx.get(), batch);
    std::vector<Status> statusVector(writerVectors.size(), Status::OK());
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (writerVectors[i].empty())
            continue;

        _writerPool->schedule([this, &writer = writerVectors.at(i), &status = statusVector.at(i)](
                                  auto scheduleStatus) {
            if (!scheduleStatus.isOK()) {
                status = scheduleStatus;
            } else {
                status = _applyOplogBatchPerWorker(&writer);
            }
        });
    }
    _writerPool->waitForIdle();

    // Make sure all the workers succeeded.
    for (const auto& status : statusVector) {
        if (!status.isOK()) {
            LOGV2_ERROR(4886012,
                        "Failed to apply operation in tenant migration",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "error"_attr = redact(status));
        }
        uassertStatusOK(status);
    }

    fpBeforeTenantOplogApplyingBatch.pauseWhileSet();

    LOGV2_DEBUG(4886011,
                1,
                "Tenant Oplog Applier starting to write no-ops",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid);
    auto lastBatchCompletedOpTimes = _writeNoOpEntries(opCtx.get(), *batch);
    stdx::lock_guard lk(_mutex);
    _lastAppliedOpTimesUpToLastBatch.donorOpTime = lastBatchCompletedOpTimes.donorOpTime;
    // If the batch contains only resume token no-ops, then the last batch completed
    // recipient optime returned will be null.
    if (!lastBatchCompletedOpTimes.recipientOpTime.isNull()) {
        _lastAppliedOpTimesUpToLastBatch.recipientOpTime =
            lastBatchCompletedOpTimes.recipientOpTime;
    }

    LOGV2_DEBUG(4886002,
                1,
                "Tenant Oplog Applier finished applying batch",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "lastBatchCompletedOpTimes"_attr = lastBatchCompletedOpTimes);

    // Notify all the waiters on optimes before and including _lastAppliedOpTimesUpToLastBatch.
    auto firstUnexpiredIter =
        _opTimeNotificationList.upper_bound(_lastAppliedOpTimesUpToLastBatch.donorOpTime);
    for (auto iter = _opTimeNotificationList.begin(); iter != firstUnexpiredIter; iter++) {
        iter->second.emplaceValue(_lastAppliedOpTimesUpToLastBatch);
    }
    _opTimeNotificationList.erase(_opTimeNotificationList.begin(), firstUnexpiredIter);

    hangInTenantOplogApplication.executeIf(
        [&](const BSONObj& data) {
            LOGV2(
                5272315,
                "hangInTenantOplogApplication failpoint enabled -- blocking until it is disabled.",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "lastBatchCompletedOpTimes"_attr = lastBatchCompletedOpTimes);
            hangInTenantOplogApplication.pauseWhileSet(opCtx.get());
        },
        [&](const BSONObj& data) { return !lastBatchCompletedOpTimes.recipientOpTime.isNull(); });
}

void TenantOplogApplier::_checkNsAndUuidsBelongToTenant(OperationContext* opCtx,
                                                        const TenantOplogBatch& batch) {
    auto checkNsAndUuid = [&](const OplogEntry& op) {
        if (!op.getNss().isEmpty() && !ClonerUtils::isNamespaceForTenant(op.getNss(), _tenantId)) {
            LOGV2_ERROR(4886015,
                        "Namespace does not belong to tenant being migrated",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "nss"_attr = op.getNss());
            uasserted(4886016, "Namespace does not belong to tenant being migrated");
        }
        if (!op.getUuid())
            return;
        if (_knownGoodUuids.find(*op.getUuid()) != _knownGoodUuids.end())
            return;
        try {
            auto nss = OplogApplierUtils::parseUUIDOrNs(opCtx, op);
            if (!ClonerUtils::isNamespaceForTenant(nss, _tenantId)) {
                LOGV2_ERROR(4886013,
                            "UUID does not belong to tenant being migrated",
                            "tenant"_attr = _tenantId,
                            "migrationUuid"_attr = _migrationUuid,
                            "UUID"_attr = *op.getUuid(),
                            "nss"_attr = nss.ns());
                uasserted(4886014, "UUID does not belong to tenant being migrated");
            }
            _knownGoodUuids.insert(*op.getUuid());
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            LOGV2_DEBUG(4886017,
                        2,
                        "UUID for tenant being migrated does not exist",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "UUID"_attr = *op.getUuid(),
                        "nss"_attr = op.getNss().ns());
        }
    };

    for (const auto& op : batch.ops) {
        if (op.expansionsEntry < 0 && !op.entry.isPartialTransaction())
            checkNsAndUuid(op.entry);
    }
    for (const auto& expansion : batch.expansions) {
        for (const auto& op : expansion) {
            checkNsAndUuid(op);
        }
    }
}

namespace {
bool isResumeTokenNoop(const OplogEntry& entry) {
    if (entry.getOpType() != OpTypeEnum::kNoop) {
        return false;
    }
    if (!entry.getObject().hasField("msg")) {
        return false;
    }
    if (entry.getObject().getStringField("msg") != TenantMigrationRecipientService::kNoopMsg) {
        return false;
    }
    return true;
}
}  // namespace

TenantOplogApplier::OpTimePair TenantOplogApplier::_writeNoOpEntries(
    OperationContext* opCtx, const TenantOplogBatch& batch) {
    auto* opObserver = cc().getServiceContext()->getOpObserver();

    // Group donor oplog entries from the same session together.
    LogicalSessionIdMap<std::vector<TenantNoOpEntry>> sessionOps;
    // All other oplog entries.
    std::vector<TenantNoOpEntry> nonSessionOps;

    // We start WriteUnitOfWork only to reserve oplog slots. So, it's ok to abort the
    // WriteUnitOfWork when it goes out of scope.
    WriteUnitOfWork wuow(opCtx);
    // Reserve oplog slots for all entries.  This allows us to write them in parallel.
    auto oplogSlots = repl::getNextOpTimes(opCtx, batch.ops.size());
    // Keep track of the greatest oplog slot actually used, ignoring resume token noops. This is
    // what we want to return from this function.
    auto greatestOplogSlotUsed = OpTime();
    auto slotIter = oplogSlots.begin();
    for (const auto& op : batch.ops) {
        if (isResumeTokenNoop(op.entry)) {
            // We do not want to set the recipient optime for resume token noop oplog entries since
            // we won't actually apply them.
            slotIter++;
            continue;
        }
        // Group oplog entries from the same session for noop writes.
        if (auto sessionId = op.entry.getOperationSessionInfo().getSessionId()) {
            sessionOps[*sessionId].emplace_back(&op.entry, slotIter);
        } else {
            nonSessionOps.emplace_back(&op.entry, slotIter);
        }
        greatestOplogSlotUsed = *slotIter++;
    }

    const size_t numOplogThreads = _writerPool->getStats().numThreads;
    const size_t numOpsPerThread = std::max(std::size_t(minOplogEntriesPerThread.load()),
                                            (nonSessionOps.size() / numOplogThreads));
    LOGV2_DEBUG(4886003,
                1,
                "Tenant Oplog Applier scheduling no-ops ",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "firstDonorOptime"_attr = batch.ops.front().entry.getOpTime(),
                "lastDonorOptime"_attr = batch.ops.back().entry.getOpTime(),
                "numOplogThreads"_attr = numOplogThreads,
                "numOpsPerThread"_attr = numOpsPerThread,
                "numOplogEntries"_attr = batch.ops.size(),
                "numSessionsInBatch"_attr = sessionOps.size());

    // Vector to store errors from each writer thread. The first numOplogThreads entries store
    // errors from the noop writes for non-session oplog entries. And the rest store errors from the
    // noop writes for each session in the batch.
    std::vector<Status> statusVector(numOplogThreads + sessionOps.size(), Status::OK());

    // Dispatch noop writes for non-session oplog entries into numOplogThreads writer threads.
    auto opsIter = nonSessionOps.begin();
    size_t numOpsRemaining = nonSessionOps.size();
    for (size_t thread = 0; thread < numOplogThreads && opsIter != nonSessionOps.end(); thread++) {
        auto numOps = std::min(numOpsPerThread, numOpsRemaining);
        if (thread == numOplogThreads - 1) {
            numOps = numOpsRemaining;
        }
        _writerPool->schedule([=, &status = statusVector.at(thread)](auto scheduleStatus) {
            if (!scheduleStatus.isOK()) {
                status = scheduleStatus;
            } else {
                try {
                    _writeNoOpsForRange(opObserver, opsIter, opsIter + numOps);
                } catch (const DBException& e) {
                    status = e.toStatus();
                }
            }
        });
        opsIter += numOps;
        numOpsRemaining -= numOps;
    }
    invariant(opsIter == nonSessionOps.end());

    // Dispatch noop writes for oplog entries from the same session into the same writer thread.
    size_t sessionThreadNum = 0;
    for (const auto& s : sessionOps) {
        _writerPool->schedule([=, &status = statusVector.at(numOplogThreads + sessionThreadNum)](
                                  auto scheduleStatus) {
            if (!scheduleStatus.isOK()) {
                status = scheduleStatus;
            } else {
                try {
                    _writeSessionNoOpsForRange(s.second.begin(), s.second.end());
                } catch (const DBException& e) {
                    status = e.toStatus();
                }
            }
        });
        sessionThreadNum++;
    }

    _writerPool->waitForIdle();

    // Make sure all the workers succeeded.
    for (const auto& status : statusVector) {
        if (!status.isOK()) {
            LOGV2_ERROR(5333900,
                        "Failed to write noop in tenant migration",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "error"_attr = redact(status));
        }
        uassertStatusOK(status);
    }
    return {batch.ops.back().entry.getOpTime(), greatestOplogSlotUsed};
}

void TenantOplogApplier::_writeSessionNoOpsForRange(
    std::vector<TenantNoOpEntry>::const_iterator begin,
    std::vector<TenantNoOpEntry>::const_iterator end) {
    auto opCtx = cc().makeOperationContext();
    tenantMigrationRecipientInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationRecipientInfo>(_migrationUuid);

    // Since the client object persists across each noop write call and the same writer thread could
    // be reused to write noop entries with older optime, we need to clear the lastOp associated
    // with the client to avoid the invariant in replClientInfo::setLastOp that the optime only goes
    // forward.
    repl::ReplClientInfo::forClient(opCtx->getClient()).clearLastOp();

    for (auto iter = begin; iter != end; iter++) {
        const auto& entry = *iter->first;
        invariant(!isResumeTokenNoop(entry));
        invariant(entry.getSessionId());

        MutableOplogEntry noopEntry;
        noopEntry.setOpType(repl::OpTypeEnum::kNoop);
        noopEntry.setNss(entry.getNss());
        noopEntry.setUuid(entry.getUuid());
        noopEntry.setObject({});  // Empty 'o' field.
        noopEntry.setObject2(entry.getEntry().toBSON());
        noopEntry.setOpTime(*iter->second);
        noopEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());

        boost::optional<MongoDOperationContextSession> scopedSession;
        boost::optional<SessionTxnRecord> sessionTxnRecord;
        if (entry.getTxnNumber() && !entry.isPartialTransaction() &&
            (entry.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction ||
             entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps)) {
            // Final applyOp for a transaction.
            auto sessionId = *entry.getSessionId();
            auto txnNumber = *entry.getTxnNumber();
            opCtx->setLogicalSessionId(sessionId);
            opCtx->setTxnNumber(txnNumber);
            opCtx->setInMultiDocumentTransaction();
            LOGV2_DEBUG(5351502,
                        1,
                        "Tenant Oplog Applier committing transaction",
                        "sessionId"_attr = sessionId,
                        "txnNumber"_attr = txnNumber,
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "op"_attr = redact(entry.toBSONForLogging()));

            // Check out the session.
            scopedSession.emplace(opCtx.get());
            auto txnParticipant = TransactionParticipant::get(opCtx.get());
            uassert(
                5351500,
                str::stream() << "Tenant oplog application failed to get transaction participant "
                                 "for transaction "
                              << txnNumber << " on session " << sessionId,
                txnParticipant);
            // We should only write the noop entry for this transaction commit once.
            uassert(5351501,
                    str::stream() << "Tenant oplog application cannot apply transaction "
                                  << txnNumber << " on session " << sessionId
                                  << " because the transaction number "
                                  << txnParticipant.getActiveTxnNumber() << " has already started",
                    txnParticipant.getActiveTxnNumber() < txnNumber);
            txnParticipant.beginOrContinueTransactionUnconditionally(opCtx.get(), txnNumber);

            // Only set sessionId and txnNumber for the final applyOp in a transaction.
            noopEntry.setSessionId(sessionId);
            noopEntry.setTxnNumber(txnNumber);

            // Use the same wallclock time as the noop entry.
            sessionTxnRecord.emplace(sessionId, txnNumber, OpTime(), noopEntry.getWallClockTime());
            sessionTxnRecord->setState(DurableTxnStateEnum::kCommitted);
        }

        // TODO(SERVER-53510) Correctly fill in pre-image and post-image op times.
        const boost::optional<OpTime> preImageOpTime = boost::none;
        const boost::optional<OpTime> postImageOpTime = boost::none;
        // TODO(SERVER-53509) Correctly fill in prevWriteOpTime for retryable writes.
        const boost::optional<OpTime> prevWriteOpTimeInTransaction = boost::none;
        noopEntry.setPreImageOpTime(preImageOpTime);
        noopEntry.setPostImageOpTime(postImageOpTime);
        noopEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);

        AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);
        writeConflictRetry(
            opCtx.get(), "writeTenantNoOps", NamespaceString::kRsOplogNamespace.ns(), [&] {
                WriteUnitOfWork wuow(opCtx.get());

                // Write the noop entry and update config.transactions.
                repl::logOp(opCtx.get(), &noopEntry);
                if (sessionTxnRecord) {
                    TransactionParticipant::get(opCtx.get())
                        .onWriteOpCompletedOnPrimary(opCtx.get(), {}, *sessionTxnRecord);
                }

                wuow.commit();
            });

        // Invalidate in-memory state so that the next time the session is checked out, it
        // would reload the transaction state from config.transactions.
        if (opCtx->inMultiDocumentTransaction()) {
            auto txnParticipant = TransactionParticipant::get(opCtx.get());
            invariant(txnParticipant);
            txnParticipant.invalidate(opCtx.get());
            opCtx->resetMultiDocumentTransactionState();
        }
    }
}

void TenantOplogApplier::_writeNoOpsForRange(OpObserver* opObserver,
                                             std::vector<TenantNoOpEntry>::const_iterator begin,
                                             std::vector<TenantNoOpEntry>::const_iterator end) {
    auto opCtx = cc().makeOperationContext();
    tenantMigrationRecipientInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationRecipientInfo>(_migrationUuid);

    // Since the client object persists across each noop write call and the same writer thread could
    // be reused to write noop entries with older optime, we need to clear the lastOp associated
    // with the client to avoid the invariant in replClientInfo::setLastOp that the optime only goes
    // forward.
    repl::ReplClientInfo::forClient(opCtx->getClient()).clearLastOp();

    AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx.get(), "writeTenantNoOps", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork wuow(opCtx.get());
            for (auto iter = begin; iter != end; iter++) {
                const auto& entry = *iter->first;
                if (isResumeTokenNoop(entry)) {
                    // We don't want to write noops for resume token noop oplog entries. They would
                    // not be applied in a change stream anyways.
                    continue;
                }
                // We don't need to link no-ops entries for operations done outside of a session.
                const boost::optional<OpTime> preImageOpTime = boost::none;
                const boost::optional<OpTime> postImageOpTime = boost::none;
                const boost::optional<OpTime> prevWriteOpTimeInTransaction = boost::none;
                opObserver->onInternalOpMessage(
                    opCtx.get(),
                    entry.getNss(),
                    entry.getUuid(),
                    {},  // Empty 'o' field.
                    entry.getEntry().toBSON(),
                    // We link the no-ops together by recipient op time the same way the actual ops
                    // were linked together by donor op time.  This is to allow retryable writes
                    // and changestreams to find the ops they need.
                    preImageOpTime,
                    postImageOpTime,
                    prevWriteOpTimeInTransaction,
                    *iter->second);
            }
            wuow.commit();
        });
}

std::vector<std::vector<const OplogEntry*>> TenantOplogApplier::_fillWriterVectors(
    OperationContext* opCtx, TenantOplogBatch* batch) {
    std::vector<std::vector<const OplogEntry*>> writerVectors(_writerPool->getStats().numThreads);
    CachedCollectionProperties collPropertiesCache;

    for (auto&& op : batch->ops) {
        // If the operation's optime is before or the same as the beginApplyingAfterOpTime we don't
        // want to apply it, so don't include it in writerVectors.
        if (op.entry.getOpTime() <= _beginApplyingAfterOpTime)
            continue;
        uassert(4886006,
                "Tenant oplog application does not support prepared transactions.",
                !op.entry.shouldPrepare());
        uassert(4886007,
                "Tenant oplog application does not support prepared transactions.",
                !op.entry.isPreparedCommit());

        // We never need to apply no-ops or partial transactions.
        if (op.entry.getOpType() == OpTypeEnum::kNoop || op.entry.isPartialTransaction())
            continue;

        if (op.expansionsEntry >= 0) {
            // This is an applyOps or transaction; add the expansions to the writer vectors.
            OplogApplierUtils::addDerivedOps(opCtx,
                                             &batch->expansions[op.expansionsEntry],
                                             &writerVectors,
                                             &collPropertiesCache,
                                             false /* serial */);
        } else {
            // Add a single op to the writer vectors.
            OplogApplierUtils::addToWriterVector(
                opCtx, &op.entry, &writerVectors, &collPropertiesCache);
        }
    }
    return writerVectors;
}

Status TenantOplogApplier::_applyOplogEntryOrGroupedInserts(
    OperationContext* opCtx,
    const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
    OplogApplication::Mode oplogApplicationMode) {
    // We must ensure the opCtx uses replicated writes, because that will ensure we get a
    // NotWritablePrimary error if a stepdown occurs.
    invariant(opCtx->writesAreReplicated());

    // Ensure context matches that of _applyOplogBatchPerWorker.
    invariant(oplogApplicationMode == OplogApplication::Mode::kInitialSync);

    auto op = entryOrGroupedInserts.getOp();
    if (op.isIndexCommandType() && op.getCommandType() != OplogEntry::CommandType::kCreateIndexes &&
        op.getCommandType() != OplogEntry::CommandType::kDropIndexes) {
        LOGV2_ERROR(488610,
                    "Index creation, except createIndex on empty collections, is not supported in "
                    "tenant migration",
                    "tenant"_attr = _tenantId,
                    "migrationUuid"_attr = _migrationUuid,
                    "op"_attr = redact(op.toBSONForLogging()));

        uasserted(5434700,
                  "Index creation, except createIndex on empty collections, is not supported in "
                  "tenant migration");
    }
    // We don't count tenant application in the ops applied stats.
    auto incrementOpsAppliedStats = [] {};
    // We always use oplog application mode 'kInitialSync', because we're applying oplog entries to
    // a cloned database the way initial sync does.
    auto status = OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
        opCtx,
        entryOrGroupedInserts,
        OplogApplication::Mode::kInitialSync,
        incrementOpsAppliedStats,
        nullptr /* opCounters*/);
    LOGV2_DEBUG(4886009,
                2,
                "Applied tenant operation",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "error"_attr = status,
                "op"_attr = redact(op.toBSONForLogging()));
    return status;
}

Status TenantOplogApplier::_applyOplogBatchPerWorker(std::vector<const OplogEntry*>* ops) {
    auto opCtx = cc().makeOperationContext();
    tenantMigrationRecipientInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationRecipientInfo>(_migrationUuid);

    // Set this to satisfy low-level locking invariants.
    opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

    const bool allowNamespaceNotFoundErrorsOnCrudOps(true);
    auto status = OplogApplierUtils::applyOplogBatchCommon(
        opCtx.get(),
        ops,
        OplogApplication::Mode::kInitialSync,
        allowNamespaceNotFoundErrorsOnCrudOps,
        [this](OperationContext* opCtx,
               const OplogEntryOrGroupedInserts& opOrInserts,
               OplogApplication::Mode mode) {
            return _applyOplogEntryOrGroupedInserts(opCtx, opOrInserts, mode);
        });
    if (!status.isOK()) {
        LOGV2_ERROR(4886008,
                    "Tenant migration writer worker batch application failed",
                    "tenant"_attr = _tenantId,
                    "migrationUuid"_attr = _migrationUuid,
                    "error"_attr = redact(status));
    }
    return status;
}

std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool() {
    return makeTenantMigrationWriterPool(tenantApplierThreadCount);
}

std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool(int threadCount) {
    return makeReplWriterPool(
        threadCount, "TenantMigrationWriter"_sd, true /*  isKillableByStepdown */);
}

}  // namespace repl
}  // namespace mongo

/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_participant.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

using Action = TransactionParticipant::StateMachine::Action;
using Event = TransactionParticipant::StateMachine::Event;
using State = TransactionParticipant::StateMachine::State;
// Server parameter that dictates the max number of milliseconds that any transaction lock request
// will wait for lock acquisition. If an operation provides a greater timeout in a lock request,
// maxTransactionLockRequestTimeoutMillis will override it. If this is set to a negative value, it
// is inactive and nothing will be overridden.
//
// 5 milliseconds will help avoid deadlocks, but will still allow fast-running metadata operations
// to run without aborting transactions.
MONGO_EXPORT_SERVER_PARAMETER(maxTransactionLockRequestTimeoutMillis, int, 5);

// Server parameter that dictates the lifetime given to each transaction.
// Transactions must eventually expire to preempt storage cache pressure immobilizing the system.
MONGO_EXPORT_SERVER_PARAMETER(transactionLifetimeLimitSeconds, std::int32_t, 60)
    ->withValidator([](const auto& potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(ErrorCodes::BadValue,
                          "transactionLifetimeLimitSeconds must be greater than or equal to 1s");
        }

        return Status::OK();
    });

namespace {

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);

MONGO_FAIL_POINT_DEFINE(hangAfterReservingPrepareTimestamp);

const auto getTransactionParticipant = Session::declareDecoration<TransactionParticipant>();

// The command names that are allowed in a multi-document transaction.
const StringMap<int> txnCmdWhitelist = {{"abortTransaction", 1},
                                        {"aggregate", 1},
                                        {"commitTransaction", 1},
                                        {"coordinateCommitTransaction", 1},
                                        {"delete", 1},
                                        {"distinct", 1},
                                        {"doTxn", 1},
                                        {"find", 1},
                                        {"findandmodify", 1},
                                        {"findAndModify", 1},
                                        {"geoSearch", 1},
                                        {"getMore", 1},
                                        {"insert", 1},
                                        {"killCursors", 1},
                                        {"prepareTransaction", 1},
                                        {"update", 1}};

// The command names that are allowed in a multi-document transaction only when test commands are
// enabled.
const StringMap<int> txnCmdForTestingWhitelist = {{"dbHash", 1}};

// The commands that can be run on the 'admin' database in multi-document transactions.
const StringMap<int> txnAdminCommands = {{"abortTransaction", 1},
                                         {"commitTransaction", 1},
                                         {"coordinateCommitTransaction", 1},
                                         {"doTxn", 1},
                                         {"prepareTransaction", 1}};

}  // unnamed namespace

TransactionParticipant* TransactionParticipant::get(OperationContext* opCtx) {
    auto session = OperationContextSession::get(opCtx);
    if (!session) {
        return nullptr;
    }

    return &getTransactionParticipant(session);
}

TransactionParticipant* TransactionParticipant::getFromNonCheckedOutSession(Session* session) {
    return &getTransactionParticipant(session);
}

const Session* TransactionParticipant::_getSession() const {
    return getTransactionParticipant.owner(this);
}

Session* TransactionParticipant::_getSession() {
    return getTransactionParticipant.owner(this);
}

void TransactionParticipant::beginOrContinue(TxnNumber txnNumber,
                                             boost::optional<bool> autocommit,
                                             boost::optional<bool> startTransaction) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (auto newState = _getSession()->getLastRefreshState()) {
        _updateState(lg, *newState);
    }

    if (txnNumber == _activeTxnNumber) {
        // It is never valid to specify 'startTransaction' on an active transaction.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Cannot specify 'startTransaction' on transaction " << txnNumber
                              << " since it is already in progress.",
                startTransaction == boost::none);

        if (_txnState.isNone(lg)) {
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot specify 'autocommit' on an operation not inside a multi-statement "
                    "transaction.",
                    autocommit == boost::none);
            return;
        }

        // Continue a multi-statement transaction. In this case, it is required that
        // autocommit=false be given as an argument on the request.

        uassert(ErrorCodes::InvalidOptions,
                "Must specify autocommit=false on all operations of a multi-statement transaction.",
                autocommit == boost::optional<bool>(false));

        if (_txnState.isInProgress(lg) && !_txnResourceStash) {
            // This indicates that the first command in the transaction failed but did not
            // implicitly abort the transaction. It is not safe to continue the transaction, in
            // particular because we have not saved the readConcern from the first statement of
            // the transaction.
            _abortTransactionOnSession(lg);
            uasserted(ErrorCodes::NoSuchTransaction,
                      str::stream() << "Transaction " << txnNumber << " has been aborted.");
        }

        return;
    }

    if (autocommit) {
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Given transaction number " << txnNumber
                              << " does not match any in-progress transactions.",
                startTransaction != boost::none);
    }

    _setNewTxnNumber(lg, txnNumber);

    _autoCommit = autocommit;
    if (!autocommit) {
        return;
    }

    // Start a multi-document transaction.
    invariant(*autocommit == false);
    _txnState.transitionTo(lg, TransactionState::kInProgress);

    // Tracks various transactions metrics.
    _singleTransactionStats.setStartTime(curTimeMicros64());
    _transactionExpireDate =
        Date_t::fromMillisSinceEpoch(_singleTransactionStats.getStartTime() / 1000) +
        stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()};

    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalStarted();
    // The transaction is considered open here and stays inactive until its first unstash event.
    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentOpen();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentInactive();

    invariant(_transactionOperations.empty());
}

void TransactionParticipant::setSpeculativeTransactionOpTimeToLastApplied(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kLastAppliedSnapshot);
    opCtx->recoveryUnit()->preallocateSnapshot();
    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
    invariant(readTimestamp);
    // Transactions do not survive term changes, so combining "getTerm" here with the
    // recovery unit timestamp does not cause races.
    _speculativeTransactionReadOpTime = {*readTimestamp, replCoord->getTerm()};
}

TransactionParticipant::OplogSlotReserver::OplogSlotReserver(OperationContext* opCtx) {
    // Stash the transaction on the OperationContext on the stack. At the end of this function it
    // will be unstashed onto the OperationContext.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Begin a new WUOW and reserve a slot in the oplog.
    WriteUnitOfWork wuow(opCtx);
    _oplogSlot = repl::getNextOpTime(opCtx);

    // Release the WUOW state since this WUOW is no longer in use.
    wuow.release();

    // The new transaction should have an empty locker, and thus we do not need to save it.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // Save the RecoveryUnit from the new transaction and replace it with an empty one.
    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TransactionParticipant::OplogSlotReserver::~OplogSlotReserver() {
    // If the constructor did not complete, we do not attempt to abort the units of work.
    if (_recoveryUnit) {
        // We should be at WUOW nesting level 1, only the top level WUOW for the oplog reservation
        // side transaction.
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
        _recoveryUnit->abortUnitOfWork();
    }
}

TransactionParticipant::TxnResources::TxnResources(OperationContext* opCtx, bool keepTicket) {
    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    if (!keepTicket) {
        _locker->releaseTicket();
    }
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

TransactionParticipant::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
        _recoveryUnit->abortUnitOfWork();
    }
}

void TransactionParticipant::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.
    _locker->reacquireTicket(opCtx);

    invariant(!_released);
    _released = true;

    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    auto oldState = opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

TransactionParticipant::SideTransactionBlock::SideTransactionBlock(OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (_opCtx->getWriteUnitOfWork()) {
        // This must be done under the client lock, since we are modifying '_opCtx'.
        stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
        _txnResources = TransactionParticipant::TxnResources(_opCtx, true /* keepTicket*/);
    }
}

TransactionParticipant::SideTransactionBlock::~SideTransactionBlock() {
    if (_txnResources) {
        // Restore the transaction state onto '_opCtx'. This must be done under the
        // client lock, since we are modifying '_opCtx'.
        stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
        _txnResources->release(_opCtx);
    }
}
void TransactionParticipant::_stashActiveTransaction(WithLock, OperationContext* opCtx) {
    invariant(_activeTxnNumber == opCtx->getTxnNumber());

    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(curTimeMicros64());
    }

    // Add the latest operation stats to the aggregate OpDebug object stored in the
    // SingleTransactionStats instance on the Session.
    _singleTransactionStats.getOpDebug()->additiveMetrics.add(
        CurOp::get(opCtx)->debug().additiveMetrics);

    invariant(!_txnResourceStash);
    _txnResourceStash = TxnResources(opCtx);

    // We accept possible slight inaccuracies in these counters from non-atomicity.
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentActive();
    ServerTransactionsMetrics::get(opCtx)->incrementCurrentInactive();

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the Session
    // with this Client's information. This is the last client that ran a transaction operation on
    // the Session.
    _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
}


void TransactionParticipant::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    // We must lock the Client to change the Locker on the OperationContext and the Session mutex to
    // access Session state. We must lock the Client before the Session mutex, since the Client
    // effectively owns the Session. That is, a user might lock the Client to ensure it doesn't go
    // away, and then lock the Session owned by that client. We rely on the fact that we are not
    // using the DefaultLockerImpl to avoid deadlock.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    stdx::unique_lock<stdx::mutex> lg(_mutex);

    // Always check session's txnNumber, since it can be modified by migration, which does not
    // check out the session. We intentionally do not error if _txnState=kAborted, since we
    // expect this function to be called at the end of the 'abortTransaction' command.
    _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

    if (!_txnState.inMultiDocumentTransaction(lg)) {
        // Not in a multi-document transaction: nothing to do.
        return;
    }

    _stashActiveTransaction(lg, opCtx);
}

void TransactionParticipant::unstashTransactionResources(OperationContext* opCtx,
                                                         const std::string& cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    {
        // We must lock the Client to change the Locker on the OperationContext and the Session
        // mutex to access Session state. We must lock the Client before the Session mutex, since
        // the Client effectively owns the Session. That is, a user might lock the Client to ensure
        // it doesn't go away, and then lock the Session owned by that client.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        // Always check session's txnNumber and '_txnState', since they can be modified by session
        // kill and migration, which do not check out the session.
        _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

        // If this is not a multi-document transaction, there is nothing to unstash.
        if (_txnState.isNone(lg)) {
            invariant(!_txnResourceStash);
            return;
        }

        // Throw NoSuchTransaction error instead of TransactionAborted error since this is the entry
        // point of transaction execution.
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been aborted.",
                !_txnState.isAborted(lg));

        // Cannot change committed transaction but allow retrying commitTransaction command.
        uassert(ErrorCodes::TransactionCommitted,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been committed.",
                cmdName == "commitTransaction" || !_txnState.isCommitted(lg));

        if (_txnResourceStash) {
            // Transaction resources already exist for this transaction.  Transfer them from the
            // stash to the operation context.

            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "Only the first command in a transaction may specify a readConcern",
                    readConcernArgs.isEmpty());
            _txnResourceStash->release(opCtx);
            _txnResourceStash = boost::none;
            // Set the starting active time for this transaction.
            if (_txnState.isInProgress(lk)) {
                _singleTransactionStats.setActive(curTimeMicros64());
            }
            // We accept possible slight inaccuracies in these counters from non-atomicity.
            ServerTransactionsMetrics::get(opCtx)->incrementCurrentActive();
            ServerTransactionsMetrics::get(opCtx)->decrementCurrentInactive();
            return;
        }

        // If we have no transaction resources then we cannot be prepared. If we're not in progress,
        // we don't do anything else.
        invariant(!_txnState.isPrepared(lk));
        if (!_txnState.isInProgress(lg)) {
            // At this point we're either committed and this is a 'commitTransaction' command, or we
            // are in the process of committing.
            return;
        }

        // Stashed transaction resources do not exist for this in-progress multi-document
        // transaction. Set up the transaction resources on the opCtx.
        opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentActive();
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentInactive();

        // Set the starting active time for this transaction.
        _singleTransactionStats.setActive(curTimeMicros64());

        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
        if (maxTransactionLockMillis >= 0) {
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }
    }

    // Storage engine transactions may be started in a lazy manner. By explicitly
    // starting here we ensure that a point-in-time snapshot is established during the
    // first operation of a transaction.
    //
    // Active transactions are protected by the locking subsystem, so we must always hold at least a
    // Global intent lock before starting a transaction.  We pessimistically acquire an intent
    // exclusive lock here because we might be doing writes in this transaction, and it is currently
    // not deadlock-safe to upgrade IS to IX.
    Lock::GlobalLock(opCtx, MODE_IX);
    opCtx->recoveryUnit()->preallocateSnapshot();

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterPreallocateSnapshot);
}

Timestamp TransactionParticipant::prepareTransaction(OperationContext* opCtx) {
    // This ScopeGuard is created outside of the lock so that the lock is always released before
    // this is called.
    ScopeGuard abortGuard = MakeGuard([&] { abortActiveTransaction(opCtx); });

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // Always check session's txnNumber and '_txnState', since they can be modified by
    // session kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    uassert(ErrorCodes::TransactionCommitted,
            str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been committed.",
            !_txnState.isCommitted(lk));

    _getSession()->lockTxnNumber(
        _activeTxnNumber,
        {ErrorCodes::PreparedTransactionInProgress,
         "cannot change transaction number while the session has a prepared transaction"});
    _txnState.transitionTo(lk, TransactionState::kPrepared);

    // Reserve an optime for the 'prepareTimestamp'. This will create a hole in the oplog and cause
    // 'snapshot' and 'afterClusterTime' readers to block until this transaction is done being
    // prepared. When the OplogSlotReserver goes out of scope and is destroyed, the
    // storage-transaction it uses to keep the hole open will abort and the slot (and corresponding
    // oplog hole) will vanish.
    OplogSlotReserver oplogSlotReserver(opCtx);
    const auto prepareOplogSlot = oplogSlotReserver.getReservedOplogSlot();
    const auto prepareTimestamp = prepareOplogSlot.opTime.getTimestamp();

    if (MONGO_FAIL_POINT(hangAfterReservingPrepareTimestamp)) {
        // This log output is used in js tests so please leave it.
        log() << "transaction - hangAfterReservingPrepareTimestamp fail point "
                 "enabled. Blocking until fail point is disabled. Prepare OpTime: "
              << prepareOplogSlot.opTime;
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterReservingPrepareTimestamp);
    }

    opCtx->recoveryUnit()->setPrepareTimestamp(prepareTimestamp);
    opCtx->getWriteUnitOfWork()->prepare();

    // We need to unlock the session to run the opObserver onTransactionPrepare, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionPrepare(opCtx, prepareOplogSlot);

    // After the oplog entry is written successfully, it is illegal to implicitly abort or fail.
    try {
        abortGuard.Dismiss();

        lk.lock();

        // Although we are not allowed to abort here, we check that we don't even try to. If we do
        // try to, that is a bug and we will fassert below.
        _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

        // Ensure that the transaction is still prepared.
        invariant(_txnState.isPrepared(lk), str::stream() << "Current state: " << _txnState);
    } catch (...) {
        severe() << "Illegal exception after transaction was prepared.";
        fassertFailedWithStatus(50906, exceptionToStatus());
    }

    return prepareTimestamp;
}

void TransactionParticipant::addTransactionOperation(OperationContext* opCtx,
                                                     const repl::ReplOperation& operation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check _getSession()'s txnNumber and '_txnState', since they can be modified by session
    // kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever add operations to an in progress transaction.
    invariant(_txnState.isInProgress(lk), str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit && !*_autoCommit && _activeTxnNumber != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    _transactionOperations.push_back(operation);
    _transactionOperationBytes += repl::OplogEntry::getReplOperationSize(operation);
    // _transactionOperationBytes is based on the in-memory size of the operation.  With overhead,
    // we expect the BSON size of the operation to be larger, so it's possible to make a transaction
    // just a bit too large and have it fail only in the commit.  It's still useful to fail early
    // when possible (e.g. to avoid exhausting server memory).
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << BSONObjMaxInternalSize
                          << ". Actual size is "
                          << _transactionOperationBytes,
            _transactionOperationBytes <= BSONObjMaxInternalSize);
}

std::vector<repl::ReplOperation> TransactionParticipant::endTransactionAndRetrieveOperations(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check session's txnNumber and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever end a transaction when prepared or in progress.
    invariant(_txnState.isInSet(lk, TransactionState::kPrepared | TransactionState::kInProgress),
              str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit);
    _transactionOperationBytes = 0;
    return std::move(_transactionOperations);
}

void TransactionParticipant::commitUnpreparedTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !_txnState.isPrepared(lk));

    // Always check session's txnNumber and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx, false /* wasPrepared */);

    lk.lock();

    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);
    // The oplog entry is written in the same WUOW with the data change for unprepared transactions.
    // We can still consider the state is InProgress until now, since no externally visible changes
    // have been made yet by the commit operation. If anything throws before this point in the
    // function, entry point will abort the transaction.
    _txnState.transitionTo(lk, TransactionState::kCommittingWithoutPrepare);
    _commitTransaction(std::move(lk), opCtx);
}

void TransactionParticipant::commitPreparedTransaction(OperationContext* opCtx,
                                                       Timestamp commitTimestamp) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction cannot provide commitTimestamp to unprepared transaction.",
            _txnState.isPrepared(lk));
    uassert(
        ErrorCodes::InvalidOptions, "'commitTimestamp' cannot be null", !commitTimestamp.isNull());

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);
    _txnState.transitionTo(lk, TransactionState::kCommittingWithPrepare);
    opCtx->recoveryUnit()->setCommitTimestamp(commitTimestamp);

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx, true /* wasPrepared */);

    lk.lock();

    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    _commitTransaction(std::move(lk), opCtx);
    _getSession()->unlockTxnNumber();
}

void TransactionParticipant::_commitTransaction(stdx::unique_lock<stdx::mutex> lk,
                                                OperationContext* opCtx) {
    auto abortGuard = MakeGuard([this, opCtx]() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _abortActiveTransaction(lock, opCtx, TransactionState::kCommittingWithoutPrepare);
    });
    lk.unlock();

    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);
    abortGuard.Dismiss();

    lk.lock();

    auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());

    // If no writes have been done, set the client optime forward to the read timestamp so waiting
    // for write concern will ensure all read data was committed.
    //
    // TODO(SERVER-34881): Once the default read concern is speculative majority, only set the
    // client optime forward if the original read concern level is "majority" or "snapshot".
    if (_speculativeTransactionReadOpTime > clientInfo.getLastOp()) {
        clientInfo.setLastOp(_speculativeTransactionReadOpTime);
    }

    _txnState.transitionTo(lk, TransactionState::kCommitted);

    // After the transaction has been committed, we must update the end time and mark it as
    // inactive.
    const auto now = curTimeMicros64();
    _singleTransactionStats.setEndTime(now);
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(now);
    }

    ServerTransactionsMetrics::get(opCtx)->incrementTotalCommitted();
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentOpen();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
    Top::get(getGlobalServiceContext())
        .incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(now));

    // Add the latest operation stats to the aggregate OpDebug object stored in the
    // SingleTransactionStats instance on the Session.
    _singleTransactionStats.getOpDebug()->additiveMetrics.add(
        CurOp::get(opCtx)->debug().additiveMetrics);

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the Session
    // with this Client's information.
    _singleTransactionStats.updateLastClientInfo(opCtx->getClient());

    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(lk,
                        &(opCtx->lockState()->getLockerInfo())->stats,
                        TransactionState::kCommitted,
                        repl::ReadConcernArgs::get(opCtx));

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    _cleanUpTxnResourceOnOpCtx(lk, opCtx);
}

void TransactionParticipant::abortArbitraryTransaction() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _abortArbitraryTransaction(lock);
}

void TransactionParticipant::abortArbitraryTransactionIfExpired() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_transactionExpireDate || _transactionExpireDate >= Date_t::now()) {
        return;
    }

    _abortArbitraryTransaction(lock);
}

void TransactionParticipant::_abortArbitraryTransaction(WithLock lock) {
    if (!_txnState.isInProgress(lock)) {
        // We do not want to abort transactions that are prepared unless we get an
        // 'abortTransaction' command.
        return;
    }

    _abortTransactionOnSession(lock);
}

void TransactionParticipant::abortActiveTransaction(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _abortActiveTransaction(
        lock, opCtx, TransactionState::kInProgress | TransactionState::kPrepared);
}

void TransactionParticipant::abortActiveUnpreparedOrStashPreparedTransaction(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // Stash the transaction if it's in prepared state.
    if (_txnState.isInSet(lock, TransactionState::kPrepared)) {
        _stashActiveTransaction(lock, opCtx);
        return;
    }
    _abortActiveTransaction(lock, opCtx, TransactionState::kInProgress);
}

void TransactionParticipant::_abortActiveTransaction(WithLock lock,
                                                     OperationContext* opCtx,
                                                     TransactionState::StateSet expectedStates) {
    invariant(!_txnResourceStash);

    if (!_txnState.isNone(lock)) {
        // Add the latest operation stats to the aggregate OpDebug object stored in the
        // SingleTransactionStats instance on the Session.
        _singleTransactionStats.getOpDebug()->additiveMetrics.add(
            CurOp::get(opCtx)->debug().additiveMetrics);

        // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
        // Session with this Client's information.
        _singleTransactionStats.updateLastClientInfo(opCtx->getClient());
    }

    // Only abort the transaction in session if it's in expected states.
    // When the state of active transaction on session is not expected, it means another
    // thread has already aborted the transaction on session.
    if (_txnState.isInSet(lock, expectedStates)) {
        invariant(opCtx->getTxnNumber() == _activeTxnNumber);
        _abortTransactionOnSession(lock);
    } else if (opCtx->getTxnNumber() == _activeTxnNumber) {
        // Cannot abort these states unless they are specified in expectedStates explicitly.
        const auto unabortableStates = TransactionState::kPrepared  //
            | TransactionState::kCommittingWithPrepare              //
            | TransactionState::kCommittingWithoutPrepare;          //
        invariant(!_txnState.isInSet(lock, unabortableStates),
                  str::stream() << "Cannot abort transaction in " << _txnState.toString());
    } else {
        // If _activeTxnNumber is higher than ours, it means the transaction is already aborted.
        invariant(_txnState.isInSet(lock, TransactionState::kNone | TransactionState::kAborted));
    }

    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(lock,
                        &(opCtx->lockState()->getLockerInfo())->stats,
                        TransactionState::kAborted,
                        repl::ReadConcernArgs::get(opCtx));

    // Clean up the transaction resources on opCtx even if the transaction on session has been
    // aborted.
    _cleanUpTxnResourceOnOpCtx(lock, opCtx);
}

void TransactionParticipant::_abortTransactionOnSession(WithLock wl) {
    const auto now = curTimeMicros64();
    if (!_txnState.isNone(wl)) {
        _singleTransactionStats.setEndTime(now);
        // The transaction has aborted, so we mark it as inactive.
        if (_singleTransactionStats.isActive()) {
            _singleTransactionStats.setInactive(now);
        }
    }

    // If the transaction is stashed, then we have aborted an inactive transaction.
    if (_txnResourceStash) {
        // The transaction is stashed, so we abort the inactive transaction on session.
        _logSlowTransaction(wl,
                            &(_txnResourceStash->locker()->getLockerInfo())->stats,
                            TransactionState::kAborted,
                            _txnResourceStash->getReadConcernArgs());
        _txnResourceStash = boost::none;
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentInactive();
    } else {
        // Transaction resource has been unstashed and transferred into an active opCtx, which will
        // clean it up.
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
    }

    _transactionOperationBytes = 0;
    _transactionOperations.clear();
    _txnState.transitionTo(wl, TransactionState::kAborted);
    _speculativeTransactionReadOpTime = repl::OpTime();

    _getSession()->unlockTxnNumber();

    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalAborted();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentOpen();

    Top::get(getGlobalServiceContext())
        .incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(now));
}

void TransactionParticipant::_cleanUpTxnResourceOnOpCtx(WithLock wl, OperationContext* opCtx) {
    // Reset the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        opCtx->setWriteUnitOfWork(nullptr);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
}

void TransactionParticipant::_checkIsActiveTransaction(WithLock wl,
                                                       const TxnNumber& requestTxnNumber,
                                                       bool checkAbort) const {
    const auto txnNumber = _getSession()->getActiveTxnNumber();
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on transaction " << _activeTxnNumber
                          << " on session "
                          << _getSession()->getSessionId()
                          << " because a different transaction "
                          << txnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on transaction " << requestTxnNumber
                          << " on session "
                          << _getSession()->getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            requestTxnNumber == _activeTxnNumber);

    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << txnNumber << " has been aborted.",
            !checkAbort || !_txnState.isAborted(wl));
}

Status TransactionParticipant::isValid(StringData dbName, StringData cmdName) {
    if (cmdName == "count"_sd) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run 'count' in a multi-document transaction. Please see "
                "http://dochub.mongodb.org/core/transaction-count for a recommended alternative."};
    }

    if (txnCmdWhitelist.find(cmdName) == txnCmdWhitelist.cend() &&
        !(getTestCommandsEnabled() &&
          txnCmdForTestingWhitelist.find(cmdName) != txnCmdForTestingWhitelist.cend())) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run '" << cmdName << "' in a multi-document transaction."};
    }

    if (dbName == "config"_sd || dbName == "local"_sd ||
        (dbName == "admin"_sd && txnAdminCommands.find(cmdName) == txnAdminCommands.cend())) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run command against the '" << dbName
                              << "' database in a transaction"};
    }

    return Status::OK();
}

BSONObj TransactionParticipant::reportStashedState() const {
    BSONObjBuilder builder;
    reportStashedState(&builder);
    return builder.obj();
}

void TransactionParticipant::reportStashedState(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_mutex);

    if (_txnResourceStash && _txnResourceStash->locker()) {
        if (auto lockerInfo = _txnResourceStash->locker()->getLockerInfo()) {
            invariant(_activeTxnNumber != kUninitializedTxnNumber);
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");

            auto lastClientInfo = _singleTransactionStats.getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);

            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                _getSession()->getSessionId().serialize(&lsid);
            }

            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                ls, &transactionBuilder, _txnResourceStash->getReadConcernArgs());

            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);

            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void TransactionParticipant::reportUnstashedState(repl::ReadConcernArgs readConcernArgs,
                                                  BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_mutex);

    if (!_txnResourceStash) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(ls, &transactionBuilder, readConcernArgs);
        builder->append("transaction", transactionBuilder.obj());
    }
}

std::string TransactionParticipant::TransactionState::toString(StateFlag state) {
    switch (state) {
        case TransactionParticipant::TransactionState::kNone:
            return "TxnState::None";
        case TransactionParticipant::TransactionState::kInProgress:
            return "TxnState::InProgress";
        case TransactionParticipant::TransactionState::kPrepared:
            return "TxnState::Prepared";
        case TransactionParticipant::TransactionState::kCommittingWithoutPrepare:
            return "TxnState::CommittingWithoutPrepare";
        case TransactionParticipant::TransactionState::kCommittingWithPrepare:
            return "TxnState::CommittingWithPrepare";
        case TransactionParticipant::TransactionState::kCommitted:
            return "TxnState::Committed";
        case TransactionParticipant::TransactionState::kAborted:
            return "TxnState::Aborted";
    }
    MONGO_UNREACHABLE;
}

bool TransactionParticipant::TransactionState::_isLegalTransition(StateFlag oldState,
                                                                  StateFlag newState) {
    switch (oldState) {
        case kNone:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kInProgress:
            switch (newState) {
                case kNone:
                case kPrepared:
                case kCommittingWithoutPrepare:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kPrepared:
            switch (newState) {
                case kCommittingWithPrepare:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommittingWithPrepare:
        case kCommittingWithoutPrepare:
            switch (newState) {
                case kNone:
                case kCommitted:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommitted:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAborted:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void TransactionParticipant::TransactionState::transitionTo(WithLock,
                                                            StateFlag newState,
                                                            TransitionValidation shouldValidate) {
    if (shouldValidate == TransitionValidation::kValidateTransition) {
        invariant(TransactionState::_isLegalTransition(_state, newState),
                  str::stream() << "Current state: " << toString(_state)
                                << ", Illegal attempted next state: "
                                << toString(newState));
    }

    _state = newState;
}

void TransactionParticipant::_reportTransactionStats(WithLock wl,
                                                     BSONObjBuilder* builder,
                                                     repl::ReadConcernArgs readConcernArgs) const {
    BSONObjBuilder parametersBuilder(builder->subobjStart("parameters"));
    parametersBuilder.append("txnNumber", _activeTxnNumber);

    if (!_txnState.inMultiDocumentTransaction(wl)) {
        // For retryable writes, we only include the txnNumber.
        parametersBuilder.done();
        return;
    }

    parametersBuilder.append("autocommit", _autoCommit ? *_autoCommit : true);
    readConcernArgs.appendInfo(&parametersBuilder);
    parametersBuilder.done();

    builder->append("readTimestamp", _speculativeTransactionReadOpTime.getTimestamp());
    builder->append("startWallClockTime",
                    dateToISOStringLocal(Date_t::fromMillisSinceEpoch(
                        _singleTransactionStats.getStartTime() / 1000)));

    // We use the same "now" time so that the following time metrics are consistent with each other.
    auto curTime = curTimeMicros64();
    builder->append("timeOpenMicros",
                    static_cast<long long>(_singleTransactionStats.getDuration(curTime)));

    auto timeActive =
        durationCount<Microseconds>(_singleTransactionStats.getTimeActiveMicros(curTime));
    auto timeInactive =
        durationCount<Microseconds>(_singleTransactionStats.getTimeInactiveMicros(curTime));

    builder->append("timeActiveMicros", timeActive);
    builder->append("timeInactiveMicros", timeInactive);

    if (_transactionExpireDate) {
        builder->append("expiryTime", dateToISOStringLocal(*_transactionExpireDate));
    }
}

void TransactionParticipant::_updateState(WithLock wl, const Session::RefreshState& newState) {
    if (newState.refreshCount <= _lastStateRefreshCount) {
        return;
    }

    _activeTxnNumber = newState.txnNumber;
    if (newState.isCommitted) {
        _txnState.transitionTo(wl,
                               TransactionState::kCommitted,
                               TransactionState::TransitionValidation::kRelaxTransitionValidation);
    }

    _lastStateRefreshCount = newState.refreshCount;
}

//
// StateMachine
//

/**
 * This table shows the events that are legal to occur (given an asynchronous network) while in each
 * state.
 *
 * For each legal event, it shows the associated action (if any) the participant should take, and
 * the next state the participant should transition to.
 *
 * Empty ("{}") transitions mean "legal event, but no action to take and no new state to transition
 * to.
 * Missing transitions are illegal.
 */
const std::map<State, std::map<Event, TransactionParticipant::StateMachine::Transition>>
    TransactionParticipant::StateMachine::transitionTable = {
        // clang-format off
        {State::kUnprepared, {
            {Event::kRecvPrepare,           {Action::kPrepare, State::kWaitingForDecision}},
            {Event::kRecvCommit,            {Action::kCommit, State::kCommitted}},
            {Event::kRecvAbort,             {Action::kAbort, State::kAborted}},
        }},
        {State::kAborted, {
            {Event::kRecvAbort,             {}},
        }},
        {State::kCommitted, {
            {Event::kRecvCommit,            {}},
        }},
        {State::kWaitingForDecision, {
            {Event::kRecvPrepare,           {}},
            {Event::kVoteCommitRejected,    {Action::kAbort, State::kAbortedAfterPrepare}},
            {Event::kRecvCommit,            {Action::kCommit, State::kCommittedAfterPrepare}},
            {Event::kRecvAbort,             {Action::kAbort, State::kAbortedAfterPrepare}},
        }},
        {State::kAbortedAfterPrepare, {
            {Event::kRecvPrepare,           {}},
            {Event::kVoteCommitRejected,    {}},
            {Event::kRecvAbort,             {}},
        }},
        {State::kCommittedAfterPrepare, {
            {Event::kRecvPrepare,           {}},
            {Event::kRecvCommit,            {}},
        }},
        {State::kBroken, {}},
        // clang-format on
};

Action TransactionParticipant::StateMachine::onEvent(Event event) {
    const auto legalTransitions = transitionTable.find(_state)->second;
    if (!legalTransitions.count(event)) {
        _state = State::kBroken;
        uasserted(ErrorCodes::InternalError,
                  str::stream() << "Transaction participant received illegal event '" << event
                                << "' while in state '"
                                << _state
                                << "'");
    }

    const auto transition = legalTransitions.find(event)->second;
    if (transition.nextState) {
        _state = *transition.nextState;
    }
    return transition.action;
}

std::string TransactionParticipant::_transactionInfoForLog(
    const SingleThreadedLockStats* lockStats,
    TransactionState::StateFlag terminationCause,
    repl::ReadConcernArgs readConcernArgs) {
    invariant(lockStats);
    invariant(terminationCause == TransactionState::kCommitted ||
              terminationCause == TransactionState::kAborted);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _getSession()->getSessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _activeTxnNumber);
    parametersBuilder.append("autocommit", _autoCommit ? *_autoCommit : true);
    readConcernArgs.appendInfo(&parametersBuilder);

    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    s << " readTimestamp:" << _speculativeTransactionReadOpTime.getTimestamp().toString() << ",";

    s << _singleTransactionStats.getOpDebug()->additiveMetrics.report();

    std::string terminationCauseString =
        terminationCause == TransactionState::kCommitted ? "committed" : "aborted";
    s << " terminationCause:" << terminationCauseString;

    auto curTime = curTimeMicros64();
    s << " timeActiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats.getTimeActiveMicros(curTime));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats.getTimeInactiveMicros(curTime));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;
    // Aggregate lock statistics.

    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    // Total duration of the transaction.
    s << " "
      << Milliseconds{static_cast<long long>(_singleTransactionStats.getDuration(curTime)) / 1000};

    return s.str();
}

void TransactionParticipant::_logSlowTransaction(WithLock wl,
                                                 const SingleThreadedLockStats* lockStats,
                                                 TransactionState::StateFlag terminationCause,
                                                 repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (!_txnState.isNone(wl)) {
        // Log the transaction if its duration is longer than the slowMS command threshold.
        if (_singleTransactionStats.getDuration(curTimeMicros64()) >
            serverGlobalParams.slowMS * 1000ULL) {
            log(logger::LogComponent::kTransaction)
                << "transaction "
                << _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
        }
    }
}

void TransactionParticipant::checkForNewTxnNumber() {
    auto txnNumber = _getSession()->getActiveTxnNumber();

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (txnNumber > _activeTxnNumber) {
        _setNewTxnNumber(lg, txnNumber);
    }
}

void TransactionParticipant::_setNewTxnNumber(WithLock wl, const TxnNumber& txnNumber) {
    invariant(!_txnState.isPrepared(wl));

    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (_txnState.isInProgress(wl)) {
        _abortTransactionOnSession(wl);
    }

    _activeTxnNumber = txnNumber;
    _txnState.transitionTo(wl, TransactionState::kNone);
    _singleTransactionStats = SingleTransactionStats();
    _speculativeTransactionReadOpTime = repl::OpTime();
    _multikeyPathInfo.clear();
    _autoCommit = boost::none;
}

}  // namespace mongo

/*
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

#include "mongo/db/operation_context_session_mongod.h"

#include "mongo/db/transaction_coordinator.h"
#include "mongo/db/transaction_coordinator_service.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {

OperationContextSessionMongod::OperationContextSessionMongod(OperationContext* opCtx,
                                                             bool shouldCheckOutSession,
                                                             boost::optional<bool> autocommit,
                                                             boost::optional<bool> startTransaction,
                                                             boost::optional<bool> coordinator)
    : _operationContextSession(opCtx, shouldCheckOutSession) {
    if (shouldCheckOutSession && !opCtx->getClient()->isInDirectClient()) {
        auto session = OperationContextSession::get(opCtx);
        invariant(session);

        auto clientTxnNumber = *opCtx->getTxnNumber();
        session->refreshFromStorageIfNeeded(opCtx);
        session->beginOrContinueTxn(opCtx, clientTxnNumber);

        if (startTransaction && *startTransaction) {
            auto clientLsid = opCtx->getLogicalSessionId().get();
            auto clockSource = opCtx->getServiceContext()->getFastClockSource();

            // If this shard has been selected as the coordinator, set up the coordinator state
            // to be ready to receive votes.
            if (coordinator && *coordinator) {
                TransactionCoordinatorService::get(opCtx)->createCoordinator(
                    clientLsid,
                    clientTxnNumber,
                    clockSource->now() + Seconds(transactionLifetimeLimitSeconds.load()));
            }
        }

        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant->beginOrContinue(clientTxnNumber, autocommit, startTransaction);
    }
}

OperationContextSessionMongodWithoutRefresh::OperationContextSessionMongodWithoutRefresh(
    OperationContext* opCtx)
    : _operationContextSession(opCtx, true /* checkout */) {
    invariant(!opCtx->getClient()->isInDirectClient());
    auto session = OperationContextSession::get(opCtx);
    invariant(session);

    auto clientTxnNumber = *opCtx->getTxnNumber();
    // Session is refreshed, but the transaction participant isn't.
    session->refreshFromStorageIfNeeded(opCtx);
    session->beginOrContinueTxn(opCtx, clientTxnNumber);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    txnParticipant->beginTransactionUnconditionally(clientTxnNumber);
}

}  // namespace mongo

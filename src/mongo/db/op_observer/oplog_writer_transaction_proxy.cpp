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

#include "mongo/db/op_observer/oplog_writer_transaction_proxy.h"

namespace mongo {

OplogWriterTransactionProxy::OplogWriterTransactionProxy(
    std::unique_ptr<OplogWriter> targetOplogWriter)
    : _targetOplogWriter(std::move(targetOplogWriter)) {}

void OplogWriterTransactionProxy::appendOplogEntryChainInfo(OperationContext* opCtx,
                                                            repl::MutableOplogEntry* oplogEntry,
                                                            repl::OplogLink* oplogLink,
                                                            const std::vector<StmtId>& stmtIds) {
    return _targetOplogWriter->appendOplogEntryChainInfo(opCtx, oplogEntry, oplogLink, stmtIds);
}

std::vector<repl::OpTime> OplogWriterTransactionProxy::logInsertOps(
    OperationContext* opCtx,
    repl::MutableOplogEntry* oplogEntryTemplate,
    std::vector<InsertStatement>::const_iterator begin,
    std::vector<InsertStatement>::const_iterator end,
    std::vector<bool> fromMigrate,
    std::function<boost::optional<ShardId>(const BSONObj& doc)> getDestinedRecipientFn,
    const CollectionPtr& collectionPtr) {
    return _targetOplogWriter->logInsertOps(opCtx,
                                            oplogEntryTemplate,
                                            begin,
                                            end,
                                            std::move(fromMigrate),
                                            getDestinedRecipientFn,
                                            collectionPtr);
}

repl::OpTime OplogWriterTransactionProxy::logOp(OperationContext* opCtx,
                                                repl::MutableOplogEntry* oplogEntry) {
    return _targetOplogWriter->logOp(opCtx, oplogEntry);
}

std::vector<OplogSlot> OplogWriterTransactionProxy::getNextOpTimes(OperationContext* opCtx,
                                                                   std::size_t count) {
    return _targetOplogWriter->getNextOpTimes(opCtx, count);
}

}  // namespace mongo

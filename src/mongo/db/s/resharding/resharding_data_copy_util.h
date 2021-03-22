/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/util/functional.h"

namespace mongo {

class NamespaceString;
class OperationContext;

namespace resharding::data_copy {

/**
 * Creates the specified collection with the given options if the collection does not already exist.
 * If the collection already exists, we do not compare the options because the resharding process
 * will always use the same options for the same namespace.
 */
void ensureCollectionExists(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options);

/**
 * Drops the specified collection or returns without error if the collection has already been
 * dropped. A particular incarnation of the collection can be dropped by specifying its UUID.
 *
 * This functions assumes the collection being dropped doesn't have any two-phase index builds
 * active on it.
 */
void ensureCollectionDropped(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<CollectionUUID>& uuid = boost::none);

Value findHighestInsertedId(OperationContext* opCtx, const CollectionPtr& collection);

/**
 * Checks out the logical session and acts in one of the following ways depending on the state of
 * this shard's config.transactions table:
 *
 *   (a) When this shard already knows about a higher transaction than txnNumber,
 *       withSessionCheckedOut() skips calling the supplied lambda function and returns boost::none.
 *
 *   (b) When this shard already knows about the retryable write statement (txnNumber, *stmtId),
 *       withSessionCheckedOut() skips calling the supplied lambda function and returns boost::none.
 *
 *   (c) When this shard has an earlier prepared transaction still active, withSessionCheckedOut()
 *       skips calling the supplied lambda function and returns a future that becomes ready once the
 *       active prepared transaction on this shard commits or aborts. After waiting for the returned
 *       future to become ready, the caller should then invoke withSessionCheckedOut() with the same
 *       arguments a second time.
 *
 *   (d) Otherwise, withSessionCheckedOut() calls the lambda function and returns boost::none.
 */
boost::optional<SharedSemiFuture<void>> withSessionCheckedOut(OperationContext* opCtx,
                                                              LogicalSessionId lsid,
                                                              TxnNumber txnNumber,
                                                              boost::optional<StmtId> stmtId,
                                                              unique_function<void()> callable);

}  // namespace resharding::data_copy
}  // namespace mongo

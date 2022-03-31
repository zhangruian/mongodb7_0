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

#pragma once

#include <memory>

#include "boost/smart_ptr/intrusive_ptr.hpp"

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/transaction_api.h"

namespace mongo {
class FLEQueryInterface;
namespace fle {

/**
 * Process a find command with encryptionInformation in-place, rewriting the filter condition so
 * that any query on an encrypted field will properly query the underlying tags array.
 */
void processFindCommand(OperationContext* opCtx,
                        FindCommandRequest* findCommand,
                        GetTxnCallback txn);

/**
 * Process a pipeline with encryptionInformation by rewriting the pipeline to query against the
 * underlying tags array, where appropriate. After this rewriting is complete, there is no more FLE
 * work to be done. The encryption info does not need to be kept around (e.g. on a command object).
 */
std::unique_ptr<Pipeline, PipelineDeleter> processPipeline(
    OperationContext* opCtx,
    NamespaceString nss,
    const EncryptionInformation& encryptInfo,
    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite);

/**
 * Rewrite a filter MatchExpression with FLE Find Payloads into a disjunction over the tag array
 * from inside an existing transaction using a FLEQueryInterface constructed from a
 * transaction client.
 */
BSONObj rewriteEncryptedFilterInsideTxn(FLEQueryInterface* queryImpl,
                                        StringData db,
                                        const EncryptedFieldConfig& efc,
                                        boost::intrusive_ptr<ExpressionContext> expCtx,
                                        BSONObj filter);

/**
 * Class which handles rewriting filter MatchExpressions for FLE2. The functionality is encapsulated
 * as a class rather than just a namespace so that the collection readers don't have to be passed
 * around as extra arguments to every function.
 *
 * Exposed in the header file for unit testing purposes. External callers should use the
 * rewriteEncryptedFilter() helper function defined below.
 */
class MatchExpressionRewrite {
public:
    /**
     * Takes in references to collection readers for the ESC and ECC that are used during tag
     * computation, along with a BSONObj holding a MatchExpression to rewrite. The rewritten
     * BSON is then retrieved by calling get() on the rewriter object.
     */
    MatchExpressionRewrite(boost::intrusive_ptr<ExpressionContext> expCtx,
                           const FLEStateCollectionReader& escReader,
                           const FLEStateCollectionReader& eccReader,
                           BSONObj filter)
        : _escReader(&escReader), _eccReader(&eccReader) {
        // This isn't the "real" query so we don't want to increment Expression
        // counters here.
        expCtx->stopExpressionCounters();
        auto expr = uassertStatusOK(MatchExpressionParser::parse(filter, expCtx));
        _result = _rewriteMatchExpression(std::move(expr))->serialize();
    }

    /**
     * Get the rewritten MatchExpression from the object.
     */
    BSONObj get() {
        return _result.getOwned();
    }

    /**
     * Determine whether a given BSONElement is in fact a FLE find payload.
     * Sub-type 6, sub-sub-type 0x05.
     */
    virtual bool isFleFindPayload(const BSONElement& elt) {
        if (!elt.isBinData(BinDataType::Encrypt)) {
            return false;
        }
        int dataLen;
        auto data = elt.binData(dataLen);
        return dataLen >= 1 &&
            data[0] == static_cast<uint8_t>(EncryptedBinDataType::kFLE2FindEqualityPayload);
    }

protected:
    /**
     * Rewrites a match expression with FLE find payloads into a disjunction on the __safeContent__
     * array of tags.
     *
     * Will rewrite top-level $eq and $in expressions, as well as recursing through $and, $or, $not
     * and $nor. All other MatchExpressions, notably $elemMatch, are ignored. This function is only
     * used directly during unit testing.
     */
    std::unique_ptr<MatchExpression> _rewriteMatchExpression(std::unique_ptr<MatchExpression> expr);

    // The default constructor should only be used for mocks in testing.
    MatchExpressionRewrite() : _escReader(nullptr), _eccReader(nullptr) {}

private:
    /**
     * A single rewrite step, called recursively on child expressions.
     */
    std::unique_ptr<MatchExpression> _rewrite(MatchExpression* me);

    virtual BSONObj rewritePayloadAsTags(BSONElement fleFindPayload);
    std::unique_ptr<InMatchExpression> rewriteEq(const EqualityMatchExpression* expr);
    std::unique_ptr<InMatchExpression> rewriteIn(const InMatchExpression* expr);

    // Holds a pointer so that these can be null for tests, even though the public constructor
    // takes a const reference.
    const FLEStateCollectionReader* _escReader;
    const FLEStateCollectionReader* _eccReader;
    BSONObj _result;
};


}  // namespace fle
}  // namespace mongo

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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/doc_validation_error_test.h"

namespace mongo::doc_validation_error {
void verifyGeneratedError(const BSONObj& query,
                          const BSONObj& document,
                          const BSONObj& expectedError) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->isParsingCollectionValidator = true;
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    MatchExpression* expr = result.getValue().get();
    BSONObj generatedError = doc_validation_error::generateError(*expr, document);

    // Verify that the document fails to match against the query.
    ASSERT_FALSE(expr->matchesBSON(document));

    // Verify that the generated error matches the expected error.
    ASSERT_BSONOBJ_EQ(generatedError, expectedError);
}

namespace {

// Comparison operators.
// $eq
TEST(ComparisonMatchExpression, BasicEq) {
    BSONObj query = BSON("a" << BSON("$eq" << 2));
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 1);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqMissingPath) {
    BSONObj query = BSON("a" << BSON("$eq" << 2));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$eq" << 2));
    BSONObj document = BSON("a" << BSON_ARRAY(3 << 4 << 5));
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(3 << 4 << 5 << BSON_ARRAY(3 << 4 << 5)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqImplicitArrayTraversalNestedDocumentSingleElement) {
    BSONObj query = BSON("a.b" << BSON("$eq" << 2));
    BSONObj document = BSON("a" << BSON_ARRAY(BSON("b" << 3)));
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 3);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqImplicitArrayTraversalNestedDocument) {
    BSONObj query = BSON("a.b" << BSON("$eq" << 2));
    BSONObj document = BSON("a" << BSON_ARRAY(BSON("b" << 3) << BSON("b" << 4) << BSON("b" << 5)));
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues" << BSON_ARRAY(3 << 4 << 5));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqImplicitArrayTraversalNestedArrays) {
    BSONObj query = BSON("a.b" << BSON("$eq" << 0));
    BSONObj document =
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1 << 2)) << BSON("b" << BSON_ARRAY(3 << 4))));
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(1 << 2 << BSON_ARRAY(1 << 2) << 3 << 4
                                                 << BSON_ARRAY(3 << 4)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, EqNoOperator) {
    BSONObj query = BSON("a" << 2);
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$eq"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 1);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $ne
TEST(ComparisonMatchExpression, BasicNe) {
    BSONObj query = BSON("a" << BSON("$ne" << 2));
    BSONObj document = BSON("a" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$ne"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison succeeded"
                                 << "consideredValue" << 2);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, NeImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$ne" << 2));
    BSONObj document = BSON("a" << BSON_ARRAY(1 << 2 << 3));
    BSONObj expectedError = BSON("operatorName"
                                 << "$ne"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison succeeded"
                                 << "consideredValues"
                                 << BSON_ARRAY(1 << 2 << 3 << BSON_ARRAY(1 << 2 << 3)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $lt
TEST(ComparisonMatchExpression, BasicLt) {
    BSONObj query = BSON("a" << BSON("$lt" << 0));
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$lt"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 1);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, LtMissingPath) {
    BSONObj query = BSON("a" << BSON("$lt" << 0));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$lt"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, LtImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$lt" << 0));
    BSONObj document = BSON("a" << BSON_ARRAY(3 << 4 << 5));
    BSONObj expectedError = BSON("operatorName"
                                 << "$lt"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(3 << 4 << 5 << BSON_ARRAY(3 << 4 << 5)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $lte
TEST(ComparisonMatchExpression, BasicLte) {
    BSONObj query = BSON("a" << BSON("$lte" << 0));
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$lte"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 1);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, LteMissingPath) {
    BSONObj query = BSON("a" << BSON("$lte" << 0));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$lte"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, LteImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$lte" << 0));
    BSONObj document = BSON("a" << BSON_ARRAY(3 << 4 << 5));
    BSONObj expectedError = BSON("operatorName"
                                 << "$lte"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(3 << 4 << 5 << BSON_ARRAY(3 << 4 << 5)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $gt
TEST(ComparisonMatchExpression, BasicGt) {
    BSONObj query = BSON("a" << BSON("$gt" << 3));
    BSONObj document = BSON("a" << 0);
    BSONObj expectedError = BSON("operatorName"
                                 << "$gt"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 0);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, GtMissingPath) {
    BSONObj query = BSON("a" << BSON("$gt" << 3));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$gt"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, GtImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$gt" << 3));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << 1 << 2));
    BSONObj expectedError = BSON("operatorName"
                                 << "$gt"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(0 << 1 << 2 << BSON_ARRAY(0 << 1 << 2)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $gte
TEST(ComparisonMatchExpression, BasicGte) {
    BSONObj query = BSON("a" << BSON("$gte" << 3));
    BSONObj document = BSON("a" << 0);
    BSONObj expectedError = BSON("operatorName"
                                 << "$gte"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValue" << 0);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, GteMissingPath) {
    BSONObj query = BSON("a" << BSON("$gte" << 3));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$gte"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, GteImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$gte" << 3));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << 1 << 2));
    BSONObj expectedError = BSON("operatorName"
                                 << "$gte"
                                 << "specifiedAs" << query << "reason"
                                 << "comparison failed"
                                 << "consideredValues"
                                 << BSON_ARRAY(0 << 1 << 2 << BSON_ARRAY(0 << 1 << 2)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $in
TEST(ComparisonMatchExpression, BasicIn) {
    BSONObj query = BSON("a" << BSON("$in" << BSON_ARRAY(1 << 2 << 3)));
    BSONObj document = BSON("a" << 4);
    BSONObj expectedError = BSON("operatorName"
                                 << "$in"
                                 << "specifiedAs" << query << "reason"
                                 << "no matching value found in array"
                                 << "consideredValue" << 4);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, InMissingPath) {
    BSONObj query = BSON("a" << BSON("$in" << BSON_ARRAY(1 << 2 << 3)));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$in"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, InNestedDocumentsAndArrays) {
    BSONObj query =
        BSON("a.b" << BSON("$in" << BSON_ARRAY(5 << 6 << 7 << BSON_ARRAY(2 << 3 << 4))));
    BSONObj document =
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1 << 2)) << BSON("b" << BSON_ARRAY(3 << 4))));
    BSONObj expectedError = BSON("operatorName"
                                 << "$in"
                                 << "specifiedAs" << query << "reason"
                                 << "no matching value found in array"
                                 << "consideredValues"
                                 << BSON_ARRAY(1 << 2 << BSON_ARRAY(1 << 2) << 3 << 4
                                                 << BSON_ARRAY(3 << 4)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $nin
TEST(ComparisonMatchExpression, BasicNin) {
    BSONObj query = BSON("a" << BSON("$nin" << BSON_ARRAY(1 << 2 << 3)));
    BSONObj document = BSON("a" << 3);
    BSONObj expectedError = BSON("operatorName"
                                 << "$nin"
                                 << "specifiedAs" << query << "reason"
                                 << "matching value found in array"
                                 << "consideredValue" << 3);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, NinNestedDocumentsAndArrays) {
    BSONObj query = BSON("a.b" << BSON("$nin" << BSON_ARRAY(1 << BSON_ARRAY(2 << 3 << 4))));
    BSONObj document =
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1 << 2)) << BSON("b" << BSON_ARRAY(3 << 4))));
    BSONObj expectedError = BSON("operatorName"
                                 << "$nin"
                                 << "specifiedAs" << query << "reason"
                                 << "matching value found in array"
                                 << "consideredValues"
                                 << BSON_ARRAY(1 << 2 << BSON_ARRAY(1 << 2) << 3 << 4
                                                 << BSON_ARRAY(3 << 4)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verify that Comparison operators which accept a regex ($in and $nin) work as expected.
TEST(ComparisonMatchExpression, InAcceptsRegex) {
    BSONObj query = BSON(
        "a" << BSON("$in" << BSON_ARRAY(BSONRegEx("^v") << BSONRegEx("^b") << BSONRegEx("^c"))));
    BSONObj document = BSON("a"
                            << "Validation");
    BSONObj expectedError = BSON("operatorName"
                                 << "$in"
                                 << "specifiedAs" << query << "reason"
                                 << "no matching value found in array"
                                 << "consideredValue"
                                 << "Validation");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ComparisonMatchExpression, NinAcceptsRegex) {
    BSONObj query = BSON(
        "a" << BSON("$nin" << BSON_ARRAY(BSONRegEx("^v") << BSONRegEx("^b") << BSONRegEx("^c"))));
    BSONObj document = BSON("a"
                            << "berry");
    BSONObj expectedError = BSON("operatorName"
                                 << "$nin"
                                 << "specifiedAs" << query << "reason"
                                 << "matching value found in array"
                                 << "consideredValue"
                                 << "berry");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Logical operators
// $and
TEST(LogicalMatchExpression, BasicAnd) {
    BSONObj failingClause = BSON("a" << BSON("$lt" << 10));
    BSONObj query = BSON("$and" << BSON_ARRAY(BSON("b" << BSON("$gt" << 0)) << failingClause));
    BSONObj document = BSON("a" << 11 << "b" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$and"
                                 << "clausesNotSatisfied"
                                 << BSON_ARRAY(BSON("index" << 1 << "details"
                                                            << BSON("operatorName"
                                                                    << "$lt"
                                                                    << "specifiedAs"
                                                                    << failingClause << "reason"
                                                                    << "comparison failed"
                                                                    << "consideredValue" << 11))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, ImplicitAnd) {
    BSONObj failingClause = BSON("a" << BSON("$lt" << 10));
    BSONObj query = BSON("a" << BSON("$gt" << 0 << "$lt" << 10));
    BSONObj document = BSON("a" << 11);
    BSONObj expectedError = BSON("operatorName"
                                 << "$and"
                                 << "clausesNotSatisfied"
                                 << BSON_ARRAY(BSON("index" << 1 << "details"
                                                            << BSON("operatorName"
                                                                    << "$lt"
                                                                    << "specifiedAs"
                                                                    << failingClause << "reason"
                                                                    << "comparison failed"
                                                                    << "consideredValue" << 11))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, AndMultipleFailingClauses) {
    BSONObj firstFailingClause = BSON("a" << BSON("$lt" << 10));
    BSONObj secondFailingClause = BSON("a" << BSON("$gt" << 20));
    BSONObj query = BSON("$and" << BSON_ARRAY(firstFailingClause << secondFailingClause));
    BSONObj document = BSON("a" << 15);
    BSONObj expectedError = BSON(
        "operatorName"
        << "$and"
        << "clausesNotSatisfied"
        << BSON_ARRAY(BSON("index" << 0 << "details"
                                   << BSON("operatorName"
                                           << "$lt"
                                           << "specifiedAs" << firstFailingClause << "reason"
                                           << "comparison failed"
                                           << "consideredValue" << 15))
                      << BSON("index" << 1 << "details"
                                      << BSON("operatorName"
                                              << "$gt"
                                              << "specifiedAs" << secondFailingClause << "reason"
                                              << "comparison failed"
                                              << "consideredValue" << 15))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NestedAndDoesNotReportErrorDetailsIfItMatches) {
    BSONObj query = fromjson("{$and: [{$and: [{a: 1}]}, {$and: [{b: 1}]}]}");
    BSONObj document = fromjson("{a: 1, b: 2}");
    BSONObj expectedError = fromjson(
        "{operatorName: '$and', clausesNotSatisfied: [{index: 1, details: {"
        "   operatorName: '$and', clausesNotSatisfied: [{index: 0, details: {"
        "       operatorName: '$eq', "
        "       specifiedAs: {b: 1}, "
        "       reason: 'comparison failed', "
        "       consideredValue: 2}}]}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $or
TEST(LogicalMatchExpression, BasicOr) {
    BSONObj failingClause = BSON("a" << BSON("$lt" << 10));
    BSONObj query = BSON("$or" << BSON_ARRAY(failingClause));
    BSONObj document = BSON("a" << 11);
    BSONObj expectedError = BSON("operatorName"
                                 << "$or"
                                 << "clausesNotSatisfied"
                                 << BSON_ARRAY(BSON("index" << 0 << "details"
                                                            << BSON("operatorName"
                                                                    << "$lt"
                                                                    << "specifiedAs"
                                                                    << failingClause << "reason"
                                                                    << "comparison failed"
                                                                    << "consideredValue" << 11))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, OrMultipleFailingClauses) {
    BSONObj firstFailingClause = BSON("a" << BSON("$lt" << 10));
    BSONObj secondFailingClause = BSON("a" << BSON("$gt" << 20));
    BSONObj query = BSON("$or" << BSON_ARRAY(firstFailingClause << secondFailingClause));
    BSONObj document = BSON("a" << 15);
    BSONObj expectedError = BSON(
        "operatorName"
        << "$or"
        << "clausesNotSatisfied"
        << BSON_ARRAY(BSON("index" << 0 << "details"
                                   << BSON("operatorName"
                                           << "$lt"
                                           << "specifiedAs" << firstFailingClause << "reason"
                                           << "comparison failed"
                                           << "consideredValue" << 15))
                      << BSON("index" << 1 << "details"
                                      << BSON("operatorName"
                                              << "$gt"
                                              << "specifiedAs" << secondFailingClause << "reason"
                                              << "comparison failed"
                                              << "consideredValue" << 15))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $nor
TEST(LogicalMatchExpression, BasicNor) {
    BSONObj firstClause = BSON("a" << BSON("$gt" << 10));
    BSONObj secondFailingClause = BSON("b" << BSON("$lt" << 10));
    BSONObj query = BSON("$nor" << BSON_ARRAY(firstClause << secondFailingClause));
    BSONObj document = BSON("a" << 9 << "b" << 9);
    BSONObj expectedError =
        BSON("operatorName"
             << "$nor"
             << "clausesNotSatisfied"
             << BSON_ARRAY(BSON("index" << 1 << "details"
                                        << BSON("operatorName"
                                                << "$lt"
                                                << "specifiedAs" << secondFailingClause << "reason"
                                                << "comparison succeeded"
                                                << "consideredValue" << 9))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NorAllSuccessfulClauses) {
    BSONObj firstFailingClause = BSON("a" << BSON("$lt" << 20));
    BSONObj secondFailingClause = BSON("a" << BSON("$gt" << 10));
    BSONObj query = BSON("$nor" << BSON_ARRAY(firstFailingClause << secondFailingClause));
    BSONObj document = BSON("a" << 15);
    BSONObj expectedError = BSON(
        "operatorName"
        << "$nor"
        << "clausesNotSatisfied"
        << BSON_ARRAY(BSON("index" << 0 << "details"
                                   << BSON("operatorName"
                                           << "$lt"
                                           << "specifiedAs" << firstFailingClause << "reason"
                                           << "comparison succeeded"
                                           << "consideredValue" << 15))
                      << BSON("index" << 1 << "details"
                                      << BSON("operatorName"
                                              << "$gt"
                                              << "specifiedAs" << secondFailingClause << "reason"
                                              << "comparison succeeded"
                                              << "consideredValue" << 15))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $not
TEST(LogicalMatchExpression, BasicNot) {
    BSONObj failingClause = BSON("$lt" << 10);
    BSONObj failingQuery = BSON("a" << failingClause);
    BSONObj query = BSON("a" << BSON("$not" << failingClause));
    BSONObj document = BSON("a" << 9);
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$lt"
                                         << "specifiedAs" << failingQuery << "reason"
                                         << "comparison succeeded"
                                         << "consideredValue" << 9));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NotOverImplicitAnd) {
    BSONObj failingQuery = BSON("$lt" << 20 << "$gt" << 5);
    BSONObj query = BSON("a" << BSON("$not" << failingQuery));
    BSONObj document = BSON("a" << 10);
    BSONObj expectedError =
        BSON("operatorName"
             << "$not"
             << "details"
             << BSON("operatorName"
                     << "$and"
                     << "clausesNotSatisfied"
                     << BSON_ARRAY(
                            BSON("index" << 0 << "details"
                                         << BSON("operatorName"
                                                 << "$lt"
                                                 << "specifiedAs" << BSON("a" << BSON("$lt" << 20))
                                                 << "reason"
                                                 << "comparison succeeded"
                                                 << "consideredValue" << 10))
                            << BSON("index" << 1 << "details"
                                            << BSON("operatorName"
                                                    << "$gt"
                                                    << "specifiedAs"
                                                    << BSON("a" << BSON("$gt" << 5)) << "reason"
                                                    << "comparison succeeded"
                                                    << "consideredValue" << 10)))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NestedNot) {
    BSONObj failingClause = BSON("$lt" << 10);
    BSONObj failingQuery = BSON("a" << failingClause);
    BSONObj query = BSON("a" << BSON("$not" << BSON("$not" << failingClause)));
    BSONObj document = BSON("a" << 11);
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$not"
                                         << "details"
                                         << BSON("operatorName"
                                                 << "$lt"
                                                 << "specifiedAs" << failingQuery << "reason"
                                                 << "comparison failed"
                                                 << "consideredValue" << 11)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Combine logical operators
TEST(LogicalMatchExpression, NestedAndOr) {
    BSONObj query = fromjson(
        "{'$and':["
        "   {'$or': "
        "       [{'price': {'$gt': 50}}, "
        "       {'price': {'$lt': 20}}]},"
        "   {'qty': {'$gt': 0}},"
        "   {'qty': {'$lt': 10}}]}");
    BSONObj document = fromjson("{'price': 30, 'qty': 30}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$and',"
        "'clausesNotSatisfied': ["
        "   {'index': 0, 'details': "
        "   {'operatorName': '$or',"
        "   'clausesNotSatisfied': ["
        "       {'index': 0, 'details': "
        "           {'operatorName': '$gt',"
        "           'specifiedAs': {'price': {'$gt': 50}},"
        "           'reason': 'comparison failed',"
        "           'consideredValue': 30}},"
        "       {'index': 1, 'details':"
        "           {'operatorName': '$lt',"
        "           'specifiedAs': {'price': {'$lt': 20}},"
        "           'reason': 'comparison failed',"
        "           'consideredValue': 30}}]}}, "
        "   {'index': 2, 'details': "
        "   {'operatorName': '$lt',"
        "    'specifiedAs': {'qty': {'$lt': 10}},"
        "    'reason': 'comparison failed',"
        "    'consideredValue': 30}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NestedAndOrOneFailingClause) {
    BSONObj query = fromjson(
        "{'$and':["
        "   {'$or':[{'price': {'$lt': 20}}]},"
        "   {'qty': {'$gt': 0}},"
        "   {'qty': {'$lt': 10}}]}");
    BSONObj document = fromjson("{'price': 15, 'qty': 30}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$and',"
        "'clausesNotSatisfied': ["
        "   {'index': 2, 'details': "
        "   {'operatorName': '$lt',"
        "    'specifiedAs': {'qty': {'$lt': 10}},"
        "    'reason': 'comparison failed',"
        "    'consideredValue': 30}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NestedAndOrNorOneSuccessfulClause) {
    BSONObj query = fromjson(
        "{'$and':["
        "   {'$or': ["
        "       {'price': {'$lt': 20}}]},"
        "   {'$nor':["
        "       {'qty': {'$gt': 20}},"
        "       {'qty': {'$lt': 20}}]}]}");
    BSONObj document = fromjson("{'price': 10, 'qty': 15}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$and',"
        "'clausesNotSatisfied': ["
        "   {'index': 1, 'details': "
        "   {'operatorName': '$nor',"
        "   'clausesNotSatisfied': ["
        "       {'index': 1, 'details':"
        "           {'operatorName': '$lt',"
        "           'specifiedAs': {'qty': {'$lt': 20}},"
        "           'reason': 'comparison succeeded',"
        "           'consideredValue': 15}}]}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(LogicalMatchExpression, NestedAndOrNorNotOneFailingClause) {
    BSONObj query = fromjson(
        "{'$and':["
        "   {'$or': ["
        "       {'price': {'$lt': 20}}]},"
        "   {'$nor':["
        "       {'qty': {'$gt': 30}},"
        "       {'qty': {'$not': {'$lt': 20}}}]}]}");
    BSONObj document = fromjson("{'price': 10, 'qty': 25}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$and',"
        "'clausesNotSatisfied': ["
        "   {'index': 1, 'details': "
        "   {'operatorName': '$nor',"
        "   'clausesNotSatisfied': ["
        "       {'index': 1, 'details':"
        "           {'operatorName': '$not',"
        "            'details':             "
        "               {'operatorName': '$lt',"
        "               'specifiedAs': {'qty': {'$lt': 20}},"
        "               'reason': 'comparison failed',"
        "               'consideredValue': 25}}}]}}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// Miscellaneous operators.
// $exists
TEST(MiscellaneousMatchExpression, BasicExists) {
    BSONObj query = BSON("a" << BSON("$exists" << true));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$exists"
                                 << "specifiedAs" << query << "reason"
                                 << "path does not exist");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, NotExists) {
    BSONObj query = BSON("a" << BSON("$exists" << false));
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$exists"
                                 << "specifiedAs" << query << "reason"
                                 << "path does exist");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// $type
TEST(MiscellaneousMatchExpression, BasicType) {
    BSONObj query = BSON("a" << BSON("$type"
                                     << "int"));
    BSONObj document = BSON("a"
                            << "one");
    BSONObj expectedError = BSON("operatorName"
                                 << "$type"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredValue"
                                 << "one"
                                 << "consideredType"
                                 << "string");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, NotType) {
    BSONObj failingClause = BSON("$type"
                                 << "string");
    BSONObj failingQuery = BSON("a" << failingClause);
    BSONObj query = BSON("a" << BSON("$not" << failingClause));
    BSONObj document = BSON("a"
                            << "words");
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$type"
                                         << "specifiedAs" << failingQuery << "reason"
                                         << "type did match"
                                         << "consideredValue"
                                         << "words"
                                         << "consideredType"
                                         << "string"));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, TypeMissingPath) {
    BSONObj query = BSON("a" << BSON("$type"
                                     << "double"));
    BSONObj document = BSON("b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$type"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, TypeImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$type"
                                     << "double"));
    BSONObj document = BSON("a" << BSON_ARRAY("x"
                                              << "y"
                                              << "z"));
    BSONObj expectedError = BSON("operatorName"
                                 << "$type"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredValues"
                                 << BSON_ARRAY("x"
                                               << "y"
                                               << "z"
                                               << BSON_ARRAY("x"
                                                             << "y"
                                                             << "z"))
                                 << "consideredTypes"
                                 << BSON_ARRAY("array"
                                               << "string"));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// $expr
TEST(MiscellaneousMatchExpression, BasicExpr) {
    BSONObj query = BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a"
                                                             << "$b")));
    BSONObj document = BSON("a" << 1 << "b" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$expr"
                                 << "specifiedAs" << query << "reason"
                                 << "$expr did not match"
                                 << "expressionResult" << false);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, NorExpr) {
    BSONObj failingClause = BSON("$eq" << BSON_ARRAY("$a"
                                                     << "$b"));
    BSONObj failingQuery = BSON("$expr" << failingClause);
    BSONObj query = BSON("$nor" << BSON_ARRAY(failingQuery));
    BSONObj document = BSON("a" << 1 << "b" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$nor"
                                 << "clausesNotSatisfied"
                                 << BSON_ARRAY(BSON(
                                        "index" << 0 << "details"
                                                << BSON("operatorName"
                                                        << "$expr"
                                                        << "specifiedAs" << failingQuery << "reason"
                                                        << "$expr did match"
                                                        << "expressionResult" << true))));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ExprImplicitArrayTraversal) {
    BSONObj query = BSON("$expr" << BSON("$eq" << BSON_ARRAY("$a"
                                                             << "$b")));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << 1 << 2) << "b" << BSON_ARRAY(3 << 4 << 5));
    BSONObj expectedError = BSON("operatorName"
                                 << "$expr"
                                 << "specifiedAs" << query << "reason"
                                 << "$expr did not match"
                                 << "expressionResult" << false);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// $mod
TEST(MiscellaneousMatchExpression, BasicMod) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("a" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "$mod did not evaluate to expected remainder"
                                 << "consideredValue" << 2);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, NotMod) {
    BSONObj failingClause = BSON("$mod" << BSON_ARRAY(2 << 0));
    BSONObj failingQuery = BSON("a" << failingClause);
    BSONObj query = BSON("a" << BSON("$not" << failingClause));
    BSONObj document = BSON("a" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$mod"
                                         << "specifiedAs" << failingQuery << "reason"
                                         << "$mod did evaluate to expected remainder"
                                         << "consideredValue" << 2));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ModMissingPath) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("b" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ModImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << 2 << 4));
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "$mod did not evaluate to expected remainder"
                                 << "consideredValues"
                                 << BSON_ARRAY(0 << 2 << 4 << BSON_ARRAY(0 << 2 << 4)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ModNonNumeric) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("a"
                            << "two");
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredType"
                                 << "string"
                                 << "expectedTypes"
                                 << BSON_ARRAY("decimal"
                                               << "double"
                                               << "int"
                                               << "long")
                                 << "consideredValue"
                                 << "two");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ModImplicitArrayTraversalNonNumeric) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("a" << BSON_ARRAY("zero"
                                              << "two"
                                              << "four"));
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredTypes"
                                 << BSON_ARRAY("array"
                                               << "string")
                                 << "expectedTypes"
                                 << BSON_ARRAY("decimal"
                                               << "double"
                                               << "int"
                                               << "long")
                                 << "consideredValues"
                                 << BSON_ARRAY("zero"
                                               << "two"
                                               << "four"
                                               << BSON_ARRAY("zero"
                                                             << "two"
                                                             << "four")));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, ModImplicitArrayTraversalMixedTypes) {
    BSONObj query = BSON("a" << BSON("$mod" << BSON_ARRAY(2 << 1)));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << "two"
                                                << "four"));
    BSONObj expectedError = BSON("operatorName"
                                 << "$mod"
                                 << "specifiedAs" << query << "reason"
                                 << "$mod did not evaluate to expected remainder"
                                 << "consideredValues"
                                 << BSON_ARRAY(0 << "two"
                                                 << "four"
                                                 << BSON_ARRAY(0 << "two"
                                                                 << "four")));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// $regex
TEST(MiscellaneousMatchExpression, BasicRegex) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("a"
                            << "one");
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "regular expression did not match"
                                 << "consideredValue"
                                 << "one");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, NotRegex) {
    BSONObj failingClause = BSON("$regex" << BSONRegEx("myRegex", "") << "$options"
                                          << "");
    BSONObj failingQuery = BSON("a" << failingClause);
    BSONObj query = BSON("a" << BSON("$not" << failingClause));
    BSONObj document = BSON("a"
                            << "myRegex");
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$regex"
                                         << "specifiedAs" << failingQuery << "reason"
                                         << "regular expression did match"
                                         << "consideredValue"
                                         << "myRegex"));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, RegexMissingPath) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("b"
                            << "myRegex");
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, RegexImplicitArrayTraversal) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("a" << BSON_ARRAY("x"
                                              << "y"
                                              << "z"));
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "regular expression did not match"
                                 << "consideredValues"
                                 << BSON_ARRAY("x"
                                               << "y"
                                               << "z"
                                               << BSON_ARRAY("x"
                                                             << "y"
                                                             << "z")));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, RegexNonString) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("a" << 1);
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredType"
                                 << "int"
                                 << "expectedTypes"
                                 << BSON_ARRAY("regex"
                                               << "string"
                                               << "symbol")
                                 << "consideredValue" << 1);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, RegexImplicitArrayTraversalNonString) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("a" << BSON_ARRAY(0 << 1 << 2));
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredTypes"
                                 << BSON_ARRAY("array"
                                               << "int")
                                 << "expectedTypes"
                                 << BSON_ARRAY("regex"
                                               << "string"
                                               << "symbol")
                                 << "consideredValues"
                                 << BSON_ARRAY(0 << 1 << 2 << BSON_ARRAY(0 << 1 << 2)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
TEST(MiscellaneousMatchExpression, RegexImplicitArrayTraversalMixedTypes) {
    BSONObj query = BSON("a" << BSON("$regex" << BSONRegEx("/myRegex/", "") << "$options"
                                              << ""));
    BSONObj document = BSON("a" << BSON_ARRAY("x" << 1 << 2));
    BSONObj expectedError = BSON("operatorName"
                                 << "$regex"
                                 << "specifiedAs" << query << "reason"
                                 << "regular expression did not match"
                                 << "consideredValues"
                                 << BSON_ARRAY("x" << 1 << 2 << BSON_ARRAY("x" << 1 << 2)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllClear expression with numeric bitmask correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllClearNumeric) {
    BSONObj query = BSON("a" << BSON("$bitsAllClear" << 2));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllClear expression with numeric bitmask correctly generates a validation
// error on unexpected match of value.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllClearNumericOnValueMatch) {
    BSONObj query = BSON("a" << BSON("$not" << BSON("$bitsAllClear" << 2)));
    BSONObj document = BSON("a" << 5);
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$bitsAllClear"
                                         << "specifiedAs" << BSON("a" << BSON("$bitsAllClear" << 2))
                                         << "reason"
                                         << "bitwise operator matched successfully"
                                         << "consideredValue" << 5));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllClear expression with position list correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllClearPositionList) {
    BSONObj query = BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(1)));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllClear expression with BinData bitmask correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllClearBinData) {
    int binaryData = 0x02;
    BSONObj query = BSON("a" << BSON("$bitsAllClear" << BSONBinData(
                                         &binaryData, sizeof(binaryData), BinDataGeneral)));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllSet expression correctly generates a validation error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllSetNumeric) {
    BSONObj query = BSON("a" << BSON("$bitsAllSet" << 2));
    BSONObj document = BSON("a" << 5);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllSet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 5);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllSet expression with position list correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllSetPositionList) {
    BSONObj query = BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(1)));
    BSONObj document = BSON("a" << 5);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllSet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 5);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllSet expression with BinData bitmask correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllSetBinData) {
    int binaryData = 0x02;
    BSONObj query = BSON(
        "a" << BSON("$bitsAllSet" << BSONBinData(&binaryData, sizeof(binaryData), BinDataGeneral)));
    BSONObj document = BSON("a" << 5);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllSet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 5);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnyClear expression correctly generates a validation error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnyClearNumeric) {
    BSONObj query = BSON("a" << BSON("$bitsAnyClear" << 3));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnyClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnyClear expression with position list correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnyClearPositionList) {
    BSONObj query = BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(1 << 0)));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnyClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnyClear expression with BinData bitmask correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnyClearBinData) {
    int binaryData = 0x03;
    BSONObj query = BSON("a" << BSON("$bitsAnyClear" << BSONBinData(
                                         &binaryData, sizeof(binaryData), BinDataGeneral)));
    BSONObj document = BSON("a" << 7);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnyClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 7);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnySet expression correctly generates a validation error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnySetNumeric) {
    BSONObj query = BSON("a" << BSON("$bitsAnySet" << 3));
    BSONObj document = BSON("a" << 0);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnySet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 0);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnySet expression with position list correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnySetPositionList) {
    BSONObj query = BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(1 << 0)));
    BSONObj document = BSON("a" << 0);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnySet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 0);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnySet expression with BinData bitmask correctly generates a validation
// error.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnySetBinData) {
    int binaryData = 0x03;
    BSONObj query = BSON(
        "a" << BSON("$bitsAnySet" << BSONBinData(&binaryData, sizeof(binaryData), BinDataGeneral)));
    BSONObj document = BSON("a" << 0);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnySet"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValue" << 0);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAnyClear expression correctly generates a validation error on value type
// mismatch.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAnyClearOnTypeMismatch) {
    BSONObj query = BSON("a" << BSON("$bitsAnyClear" << 3));
    BSONObj document = BSON("a"
                            << "someString");
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAnyClear"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredType"
                                 << "string"
                                 << "expectedTypes"
                                 << BSON_ARRAY("binData"
                                               << "decimal"
                                               << "double"
                                               << "int"
                                               << "long")
                                 << "consideredValue"
                                 << "someString");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $bitsAllClear expression with numeric bitmask correctly generates a validation
// error when applied on an array of numeric values.
TEST(BitTestMatchExpression, GeneratesValidationErrorBitsAllClearOnValueArray) {
    BSONObj query = BSON("a" << BSON("$bitsAllClear" << 2));
    BSONArray attributeValue = BSON_ARRAY(7 << 3);
    BSONObj document = BSON("a" << attributeValue);
    BSONObj expectedError = BSON("operatorName"
                                 << "$bitsAllClear"
                                 << "specifiedAs" << query << "reason"
                                 << "bitwise operator failed to match"
                                 << "consideredValues" << BSON_ARRAY(7 << 3 << attributeValue));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoIntersects expression correctly generates a validation error.
TEST(GeoMatchExpression, GeneratesValidationErrorGeoIntersects) {
    BSONObj query = fromjson(
        "{'a': {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, "
        "0], [0, 0]]]}}}}");
    BSONObj point = BSON("type"
                         << "Point"
                         << "coordinates" << BSON_ARRAY(3 << 3));
    BSONObj document = BSON("a" << point);
    BSONObj expectedError =
        BSON("operatorName"
             << "$geoIntersects"
             << "specifiedAs" << query << "reason"
             << "none of considered geometries intersected the expression’s geometry"
             << "consideredValue" << point);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoIntersects expression correctly generates a validation error on unexpected
// match of value.
TEST(GeoMatchExpression, GeneratesValidationErrorGeoIntersectsOnValueMatch) {
    BSONObj subquery = fromjson(
        "{$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, 0], [0, "
        "0]]]}}}");
    BSONObj query = BSON("a" << BSON("$not" << subquery));
    BSONObj point = BSON("type"
                         << "Point"
                         << "coordinates" << BSON_ARRAY(1 << 1));
    BSONObj document = BSON("a" << point);
    BSONObj expectedError =
        BSON(
            "operatorName"
            << "$not"
            << "details"
            << BSON("operatorName"
                    << "$geoIntersects"
                    << "specifiedAs" << BSON("a" << subquery) << "reason"
                    << "at least one of considered geometries intersected the expression’s geometry"
                    << "consideredValue" << point));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoIntersects expression correctly generates a correct validation error on value
// type mismatch.
TEST(GeoMatchExpression, GeneratesValidationErrorGeoIntersectsOnTypeMismatch) {
    BSONObj query = fromjson(
        "{'a': {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, "
        "0], [0, 0]]]}}}}");
    BSONObj point = BSON("type"
                         << "Point"
                         << "coordinates" << BSON_ARRAY(3 << 3));
    BSONObj document = BSON("a" << 2);
    BSONObj expectedError = BSON("operatorName"
                                 << "$geoIntersects"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredType"
                                 << "int"
                                 << "expectedTypes"
                                 << BSON_ARRAY("array"
                                               << "object")
                                 << "consideredValue" << 2);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoIntersects expression correctly generates a validation error when applied on an
// array of points.
TEST(GeoMatchExpression, GeneratesValidationErrorGeoIntersectsOnValueArray) {
    BSONObj query = fromjson(
        "{'a': {$geoIntersects: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, "
        "0], [0, 0]]]}}}}");
    BSONObj point1 = BSON("type"
                          << "Point"
                          << "coordinates" << BSON_ARRAY(3 << 3));
    BSONObj point2 = BSON("type"
                          << "Point"
                          << "coordinates" << BSON_ARRAY(4 << 4));
    auto points = BSON_ARRAY(point1 << point2);
    BSONObj document = BSON("a" << points);
    BSONObj expectedError =
        BSON("operatorName"
             << "$geoIntersects"
             << "specifiedAs" << query << "reason"
             << "none of considered geometries intersected the expression’s geometry"
             << "consideredValues" << BSON_ARRAY(point1 << point2 << points));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoWithin expression correctly generates a validation error.
TEST(GeoMatchExpression, GeneratesValidationErrorGeoWithin) {
    BSONObj query = fromjson(
        "{'a': {$geoWithin: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, 0], "
        "[0, 0]]]}}}}");
    BSONObj point = BSON("type"
                         << "Point"
                         << "coordinates" << BSON_ARRAY(3 << 3));
    BSONObj document = BSON("a" << point);
    BSONObj expectedError =
        BSON("operatorName"
             << "$geoWithin"
             << "specifiedAs" << query << "reason"
             << "none of considered geometries was contained within the expression’s geometry"
             << "consideredValue" << point);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// Verifies that $geoWithin expression correctly generates an inverse validation error.
TEST(GeoMatchExpression, GeneratesValidationErrorForMatchGeoWithin) {
    BSONObj subquery = fromjson(
        "{$geoWithin: {$geometry: {type: 'Polygon', coordinates: [[[0, 0], [0, 3], [3, 0], [0, "
        "0]]]}}}");
    BSONObj query = BSON("a" << BSON("$not" << subquery));
    BSONObj point = BSON("type"
                         << "Point"
                         << "coordinates" << BSON_ARRAY(1 << 1));
    BSONObj document = BSON("a" << point);
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$geoWithin"
                                         << "specifiedAs" << BSON("a" << subquery) << "reason"
                                         << "at least one of considered geometries was contained "
                                            "within the expression’s geometry"
                                         << "consideredValue" << point));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
// Array operators.

// $size

TEST(ArrayMatchingMatchExpression, BasicSize) {
    BSONObj query = BSON("a" << BSON("$size" << 2));
    BSONObj document = BSON("a" << BSON_ARRAY(1 << 2 << 3));
    BSONObj expectedError = BSON("operatorName"
                                 << "$size"
                                 << "specifiedAs" << query << "reason"
                                 << "array length was not equal to given size"
                                 << "consideredValue" << BSON_ARRAY(1 << 2 << 3));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, SizeNonArray) {
    BSONObj query = BSON("a" << BSON("$size" << 2));
    BSONObj document = BSON("a" << 3);
    BSONObj expectedError = BSON("operatorName"
                                 << "$size"
                                 << "specifiedAs" << query << "reason"
                                 << "type did not match"
                                 << "consideredType"
                                 << "int"
                                 << "expectedType"
                                 << "array"
                                 << "consideredValue" << 3);
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}


TEST(ArrayMatchingMatchExpression, SizeMissingPath) {
    BSONObj query = BSON("a" << BSON("$size" << 2));
    BSONObj document = BSON("b" << 3);
    BSONObj expectedError = BSON("operatorName"
                                 << "$size"
                                 << "specifiedAs" << query << "reason"
                                 << "field was missing");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, NotOverSize) {
    BSONObj query = BSON("a" << BSON("$not" << BSON("$size" << 2)));
    BSONObj document = BSON("a" << BSON_ARRAY(1 << 2));
    BSONObj expectedError = BSON("operatorName"
                                 << "$not"
                                 << "details"
                                 << BSON("operatorName"
                                         << "$size"
                                         << "specifiedAs" << BSON("a" << BSON("$size" << 2))
                                         << "reason"
                                         << "array length was equal to given size"
                                         << "consideredValue" << BSON_ARRAY(1 << 2)));
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $all
TEST(ArrayMatchingMatchExpression, BasicAll) {
    BSONObj query = fromjson("{'a': {'$all': [1,2,3]}}");
    BSONObj document = fromjson("{'a': [1,2,4]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$all',"
        "'specifiedAs': {'a': {'$all': [1,2,3]}},"
        "'reason': 'array did not contain all specified values',"
        "'consideredValue': [1,2,4]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, AllRegex) {
    BSONObj query = fromjson("{'a': {'$all': [/^a/,/^b/]}}");
    BSONObj document = fromjson("{'a': ['abc', 'cbc']}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$all',"
        "'specifiedAs': {'a': {'$all': [/^a/,/^b/]}},"
        "'reason': 'array did not contain all specified values',"
        "'consideredValue': ['abc', 'cbc']}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, AllMissingPath) {
    BSONObj query = fromjson("{'a': {'$all': [1,2,3]}}");
    BSONObj document = fromjson("{'b': [1,2,3]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$all',"
        "'specifiedAs': {'a': {'$all': [1,2,3]}},"
        "'reason': 'field was missing'}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, AllNoValues) {
    BSONObj query = fromjson("{'a': {'$all': []}}");
    BSONObj document = fromjson("{'a': [1,2,3]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$all',"
        "'specifiedAs': {'a': {'$all': []}},"
        "'reason': 'expression always evaluates to false'}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, NotOverAll) {
    BSONObj query = fromjson("{'a': {'$not': {'$all': [1,2,3]}}}");
    BSONObj document = fromjson("{'a': [1,2,3]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$not',"
        "'details':"
        "   {'operatorName': '$all',"
        "   'specifiedAs': {'a': {'$all': [1,2,3]}},"
        "   'reason': 'array did contain all specified values',"
        "   'consideredValue': [1,2,3]}}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $elemMatch
TEST(ArrayMatchingMatchExpression, BasicElemMatchValue) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'$gt': 0,'$lt': 10}}}");
    BSONObj document = fromjson("{'a': [10,11,12]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a':{'$elemMatch':{'$gt': 0,'$lt': 10}}},"
        "'reason': 'array did not satisfy the child predicate',"
        "'consideredValue': [10,11,12]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, ElemMatchValueMissingPath) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'$gt': 0,'$lt': 10}}}");
    BSONObj document = fromjson("{'b': [10,11,12]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a':{'$elemMatch':{'$gt': 0,'$lt': 10}}},"
        "'reason': 'field was missing'}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, ElemMatchValueNonArray) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'$gt': 0,'$lt': 10}}}");
    BSONObj document = fromjson("{'a': 5}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a':{'$elemMatch':{'$gt': 0,'$lt': 10}}},"
        "'reason': 'type did not match',"
        "'consideredType': 'int',"
        "'expectedType': 'array',"
        "'consideredValue': 5}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, NotOverElemMatchValue) {
    BSONObj query = fromjson("{'a': {'$not': {'$elemMatch': {'$gt': 0,'$lt': 10}}}}");
    BSONObj document = fromjson("{'a': [3,4,5]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$not', "
        "'details': {'operatorName': '$elemMatch',"
        "   'specifiedAs': {'a':{'$elemMatch':{'$gt': 0,'$lt': 10}}},"
        "   'reason': 'array did satisfy the child predicate',"
        "   'consideredValue': [3,4,5]}}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, BasicElemMatchObject) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}}");
    BSONObj document = fromjson("{'a': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}},"
        "'reason': 'array did not satisfy the child predicate',"
        "'consideredValue': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, ElemMatchObjectMissingPath) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}}");
    BSONObj document = fromjson("{'b': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}},"
        "'reason': 'field was missing'}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, ElemMatchObjectNonArray) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}}");
    BSONObj document = fromjson("{'a': 'foo'}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a': {'$elemMatch': {'b': {'$gt': 0}, 'c': {'$lt': 0}}}},"
        "'reason': 'type did not match',"
        "'consideredType': 'string',"
        "'expectedType': 'array',"
        "'consideredValue': 'foo'}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, NestedElemMatchObject) {
    BSONObj query = fromjson("{'a': {'$elemMatch': {'b': {$elemMatch: {'c': {'$lt': 0}}}}}}");
    BSONObj document = fromjson("{'a': [{'b': [{'c': [1,2,3]}, {'c': [4,5,6]}]}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$elemMatch',"
        "'specifiedAs': {'a': {'$elemMatch': {'b': {$elemMatch: {'c': {'$lt': 0}}}}}},"
        "'reason': 'array did not satisfy the child predicate',"
        "'consideredValue': [{'b': [{'c': [1,2,3]}, {'c': [4,5,6]}]}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

TEST(ArrayMatchingMatchExpression, NotOverElemMatchObject) {
    BSONObj query =
        fromjson("{'a': {'$not': {'$elemMatch': {'b': {'$gte': 0}, 'c': {'$lt': 10}}}}}");
    BSONObj document = fromjson("{'a': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$not', "
        "'details': {'operatorName': '$elemMatch',"
        "   'specifiedAs': {'a':{'$elemMatch': {'b': {'$gte': 0}, 'c': {'$lt': 10}}}},"
        "   'reason': 'array did satisfy the child predicate',"
        "   'consideredValue': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}

// $all and $elemMatch
TEST(ArrayMatchingMatchExpression, AllOverElemMatch) {
    BSONObj query = fromjson(
        "{'a': {$all: ["
        "  {'$elemMatch': {'b': {'$gte': 0}}},"
        "  {'$elemMatch': {'c': {'$lt': 0}}}]}}");
    BSONObj document = fromjson("{'a': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    BSONObj expectedError = fromjson(
        "{'operatorName': '$all',"
        "'specifiedAs': {'a': {'$all': "
        "   [{'$elemMatch': {'b': {'$gte': 0}}}, {'$elemMatch': {'c': {'$lt': 0}}}]}},"
        "'reason': 'array did not contain all specified values',"
        "'consideredValue': [{'b': 0, 'c': 0}, {'b': 1, 'c': 1}]}");
    doc_validation_error::verifyGeneratedError(query, document, expectedError);
}
}  // namespace
}  // namespace mongo::doc_validation_error

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

#include <boost/intrusive_ptr.hpp>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using namespace std::string_literals;

auto getExpCtx() {
    auto nss = NamespaceString{"db", "coll"};
    return boost::intrusive_ptr<ExpressionContextForTest>{new ExpressionContextForTest(nss)};
}

TEST(CstPipelineTranslationTest, TranslatesEmpty) {
    const auto cst = CNode{CNode::ArrayChildren{}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(0u, sources.size());
}

TEST(CstPipelineTranslationTest, TranslatesEmptyProject) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{
        CNode::ObjectChildren{{KeyFieldname::projectInclusion, CNode{CNode::ObjectChildren{}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter));
}

TEST(CstPipelineTranslationTest, TranslatesEmptyProjects) {
    const auto cst =
        CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{{KeyFieldname::projectInclusion,
                                                                CNode{CNode::ObjectChildren{}}}}},
                                   CNode{CNode::ObjectChildren{{KeyFieldname::projectInclusion,
                                                                CNode{CNode::ObjectChildren{}}}}},
                                   CNode{CNode::ObjectChildren{{KeyFieldname::projectInclusion,
                                                                CNode{CNode::ObjectChildren{}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(3u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter++));
    ASSERT(typeid(DocumentSourceSingleDocumentTransformation) == typeid(**iter));
}

TEST(CstPipelineTranslationTest, TranslatesOneFieldInclusionProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultifieldInclusionProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{{KeyFieldname::id, CNode{KeyValue::trueKey}},
                                     {UserFieldname{"a"}, CNode{NonZeroKey{7}}},
                                     {UserFieldname{"b"}, CNode{NonZeroKey{-99999999999ll}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a" << true << "b" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesCompoundObjectInclusionProjection) {
    // [
    //     { $project: { a: { b: {
    //         c: true,
    //         d: 88,
    //         e: { f: NumberLong(-3) },
    //     } } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CompoundInclusionKey{CNode{CNode::ObjectChildren{
                  {{UserFieldname{"b"},
                    CNode{CNode::ObjectChildren{
                        {{UserFieldname{"c"}, CNode{KeyValue::trueKey}},
                         {UserFieldname{"d"}, CNode{CNode{NonZeroKey{88}}}},
                         {UserFieldname{"e"},
                          CNode{CNode::ObjectChildren{
                              {{UserFieldname{"f"}, CNode{NonZeroKey{-3ll}}}}}}}}}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("b" << BSON("c" << true << "d" << true << "e" << BSON("f" << true)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesOneFieldExclusionProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectExclusion,
         CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::falseKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("a" << false) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultifieldExclusionProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectExclusion,
         CNode{CNode::ObjectChildren{{KeyFieldname::id, CNode{KeyValue::falseKey}},
                                     {UserFieldname{"a"}, CNode{KeyValue::doubleZeroKey}},
                                     {UserFieldname{"b"}, CNode{KeyValue::decimalZeroKey}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << false << "a" << false << "b" << false) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesCompoundObjectExclusionProjection) {
    // [
    //     { $project: { a: { b: {
    //         c: false,
    //         d: 0,
    //         e: { f: NumberLong(0) },
    //     } } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectExclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CompoundExclusionKey{CNode{CNode::ObjectChildren{
                  {{UserFieldname{"b"},
                    CNode{CNode::ObjectChildren{
                        {{UserFieldname{"c"}, CNode{KeyValue::falseKey}},
                         {UserFieldname{"d"}, CNode{CNode{NonZeroKey{0}}}},
                         {UserFieldname{"e"},
                          CNode{CNode::ObjectChildren{
                              {{UserFieldname{"f"}, CNode{NonZeroKey{0ll}}}}}}}}}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("a" << BSON("b" << BSON("c" << false << "d" << false << "e" << BSON("f" << false)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjection) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::atan2,
                   CNode{CNode::ArrayChildren{CNode{UserInt{1}}, CNode{UserInt{0}}}}}}}},
             {UserFieldname{"b"},
              CNode{
                  CNode::ObjectChildren{{KeyFieldname::add,
                                         CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                                    CNode{UserInt{2}},
                                                                    CNode{UserInt{3}},
                                                                    CNode{UserInt{4}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$atan2" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 0)))
                   << "b"
                   << BSON("$add" << BSON_ARRAY(BSON("$const" << 1)
                                                << BSON("$const" << 2) << BSON("$const" << 3)
                                                << BSON("$const" << 4)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesComputedInclusionMixedProjectionStage) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::add,
                   CNode{CNode::ArrayChildren{CNode{UserLong{0ll}}, CNode{UserInt{1}}}}}}}},
             {UserFieldname{"b"}, CNode{NonZeroKey{Decimal128{590.095}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$add" << BSON_ARRAY(BSON("$const" << 0ll) << BSON("$const" << 1)))
                   << "b" << true) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesMultipleProjectionStages) {
    // [
    //     { $project: { a: true },
    //     { $project: { b: false },
    //     { $project: { c: { $add: [
    //         { $const: 2.2 },
    //         { $atan2: [ { $const: 1 }, { $const: 0 } ] },
    //         { $const: 3 } ] } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{
            {KeyFieldname::projectInclusion,
             CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}}}}}}},
        CNode{CNode::ObjectChildren{
            {KeyFieldname::projectExclusion,
             CNode{CNode::ObjectChildren{{UserFieldname{"b"}, CNode{KeyValue::falseKey}}}}}}},
        CNode{
            CNode::ObjectChildren{{KeyFieldname::projectInclusion,
                                   CNode{CNode::ObjectChildren{
                                       {UserFieldname{"c"},
                                        CNode{CNode::ObjectChildren{
                                            {KeyFieldname::add,
                                             CNode{CNode::ArrayChildren{
                                                 CNode{UserDouble{2.2}},
                                                 CNode{CNode::ObjectChildren{
                                                     {KeyFieldname::atan2,
                                                      CNode{CNode::ArrayChildren{
                                                          CNode{UserInt{1}}, CNode{UserInt{0}}}}}}},
                                                 CNode{UserLong{3ll}}}}}}}}}}}}},
    }};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(3u, sources.size());
    auto iter = sources.begin();
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << true) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("b" << false) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "c"
                       << BSON("$add"
                               << BSON_ARRAY(BSON("$const" << 2.2)
                                             << BSON("$atan2" << BSON_ARRAY(BSON("$const" << 1)
                                                                            << BSON("$const" << 0)))
                                             << BSON("$const" << 3ll)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
}

TEST(CstPipelineTranslationTest, TranslatesMultipleProjectionStagesWithAndOrNot) {
    // [
    //     { $project: { a: { $not: [
    //         { $const: 0 } },
    //     { $project: { c: { $and: [
    //         { $const: 2.2 },
    //         { $or: [ { $const: 1 }, { $const: 0 } ] },
    //         { $const: 3 } ] } } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{
            {KeyFieldname::projectInclusion,
             CNode{CNode::ObjectChildren{
                 {UserFieldname{"a"},
                  CNode{CNode::ObjectChildren{
                      {KeyFieldname::notExpr,
                       CNode{CNode::ArrayChildren{CNode{UserInt{0}}}}}}}}}}}}},
        CNode{
            CNode::ObjectChildren{{KeyFieldname::projectInclusion,
                                   CNode{CNode::ObjectChildren{
                                       {UserFieldname{"c"},
                                        CNode{CNode::ObjectChildren{
                                            {KeyFieldname::andExpr,
                                             CNode{CNode::ArrayChildren{
                                                 CNode{UserDouble{2.2}},
                                                 CNode{CNode::ObjectChildren{
                                                     {KeyFieldname::orExpr,
                                                      CNode{CNode::ArrayChildren{
                                                          CNode{UserInt{1}}, CNode{UserInt{0}}}}}}},
                                                 CNode{UserLong{3ll}}}}}}}}}}}}},
    }};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(2u, sources.size());
    auto iter = sources.begin();
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter++);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "a" << BSON("$not" << BSON_ARRAY(BSON("$const" << 0)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
    {
        auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
        // DocumenSourceSingleDocumentTransformation reorders fields so we need to be
        // insensitive.
        ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
            BSON("_id" << true << "c"
                       << BSON("$and"
                               << BSON_ARRAY(BSON("$const" << 2.2)
                                             << BSON("$or" << BSON_ARRAY(BSON("$const" << 1)
                                                                         << BSON("$const" << 0)))
                                             << BSON("$const" << 3ll)))) ==
            singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
    }
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjectionWithAndOr) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::andExpr,
                   CNode{CNode::ArrayChildren{
                       CNode{UserInt{1}},
                       CNode{CNode::ObjectChildren{
                           {KeyFieldname::add,
                            CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                       CNode{UserInt{0}}}}}}}}}}}}},
             {UserFieldname{"b"},
              CNode{
                  CNode::ObjectChildren{{KeyFieldname::orExpr,
                                         CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                                    CNode{UserInt{2}},
                                                                    CNode{UserInt{3}},
                                                                    CNode{UserInt{4}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$and" << BSON_ARRAY(BSON("$const" << 1) << BSON(
                                                    "$add" << BSON_ARRAY(BSON("$const" << 1)
                                                                         << BSON("$const" << 0)))))
                   << "b"
                   << BSON("$or" << BSON_ARRAY(BSON("$const" << 1)
                                               << BSON("$const" << 2) << BSON("$const" << 3)
                                               << BSON("$const" << 4)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesComputedProjectionWithExpressionOnId) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {KeyFieldname::id,
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::add,
                   CNode{CNode::ArrayChildren{
                       CNode{UserInt{0}},
                       CNode{CNode::ObjectChildren{
                           {KeyFieldname::andExpr,
                            CNode{CNode::ArrayChildren{CNode{UserInt{1}},
                                                       CNode{UserInt{0}}}}}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << BSON(
                 "$add" << BSON_ARRAY(
                     BSON("$const" << 0)
                     << BSON("$and" << BSON_ARRAY(BSON("$const" << 1) << BSON("$const" << 0)))))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithInt) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithDouble) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserDouble{5.5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 5ll);
}

TEST(CstPipelineTranslationTest, TranslatesSkipWithLong) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserLong{8223372036854775807}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ((dynamic_cast<DocumentSourceSkip&>(**iter).getSkip()), 8223372036854775807);
}

TEST(CstPipelineTranslationTest, FailsToTranslateSkipWithNegativeValue) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{-1}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15956);
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithInt) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::skip, CNode{UserInt{0}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceSkip) == typeid(**iter));
    ASSERT_EQ(0ll, dynamic_cast<DocumentSourceSkip&>(**iter).getSkip());
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithDouble) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserDouble{10.5}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(10ll, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstPipelineTranslationTest, TranslatesLimitWithLong) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserLong{123123123123}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    ASSERT(typeid(DocumentSourceLimit) == typeid(**iter));
    ASSERT_EQ(123123123123, dynamic_cast<DocumentSourceLimit&>(**iter).getLimit());
}

TEST(CstPipelineTranslationTest, FailsToTranslateLimitWithZeroKey) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{0}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15958);
}

TEST(CstPipelineTranslationTest, FailsToTranslateLimitWithNegativeValue) {
    const auto cst = CNode{CNode::ArrayChildren{
        CNode{CNode::ObjectChildren{{KeyFieldname::limit, CNode{UserInt{-1}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 15958);
}

TEST(CstPipelineTranslationTest, TranslatesSampleWithValidSize) {
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserLong{5}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(5ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserDouble{5.8}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(5ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
    {
        const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
            {KeyFieldname::sample,
             CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserLong{0}}}}}}}}}};
        auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
        auto& sources = pipeline->getSources();
        ASSERT_EQ(1u, sources.size());
        auto iter = sources.begin();
        ASSERT(typeid(DocumentSourceSample) == typeid(**iter));
        ASSERT_EQ(0ll, dynamic_cast<DocumentSourceSample&>(**iter).getSampleSize());
    }
}

TEST(CstPipelineTranslationTest, FailsToTranslateSampleWithNegativeSize) {
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::sample,
         CNode{CNode::ObjectChildren{{KeyFieldname::sizeArg, CNode{UserInt{-1}}}}}}}}}};
    ASSERT_THROWS_CODE(
        cst_pipeline_translation::translatePipeline(cst, getExpCtx()), DBException, 28747);
}

TEST(CstPipelineTranslationTest, TranslatesCmpExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::cmp,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::CMP, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesEqExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::eq,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::EQ, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesGtExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::gt,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::GT, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesGteExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::gte,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::GTE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesLtExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::lt,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::LT, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesLteExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::lte,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::LTE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesNeExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::ne,
         CNode{CNode::ArrayChildren{CNode{UserLong{1}}, CNode{UserDouble{2.5}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionCompare*>(expr.get()));
    ASSERT_EQ(ExpressionCompare::CmpOp::NE, dynamic_cast<ExpressionCompare*>(expr.get())->getOp());
}

TEST(CstPipelineTranslationTest, TranslatesProjectionWithConvert) {
    // [
    //     { $project: { a: { $convert: { input: "true", to: "bool"} },
    //                  { b: { $convert: { input: 1.999999, to: "int",
    //                                     onError: "Can't convert", onNull: NumberInt("1") } } }
    //     } }
    // ]
    const auto cst = CNode{CNode::ArrayChildren{CNode{CNode::ObjectChildren{
        {KeyFieldname::projectInclusion,
         CNode{CNode::ObjectChildren{
             {UserFieldname{"a"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::convert,
                   CNode{CNode::ObjectChildren{
                       {KeyFieldname::inputArg, CNode{UserBoolean{true}}},
                       {KeyFieldname::toArg, CNode{UserString{"bool"}}},
                       {KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}},
                       {KeyFieldname::onNullArg, CNode{KeyValue::absentKey}}}}}}}},
             {UserFieldname{"b"},
              CNode{CNode::ObjectChildren{
                  {KeyFieldname::convert,
                   CNode{CNode::ObjectChildren{
                       {KeyFieldname::inputArg, CNode{UserDouble{1.999999}}},
                       {KeyFieldname::toArg, CNode{UserString{"int"}}},
                       {KeyFieldname::onErrorArg, CNode{UserString{"Can't convert"}}},
                       {KeyFieldname::onNullArg, CNode{UserInt{1}}}}}}}}}}}}}}}};
    auto pipeline = cst_pipeline_translation::translatePipeline(cst, getExpCtx());
    auto& sources = pipeline->getSources();
    ASSERT_EQ(1u, sources.size());
    auto iter = sources.begin();
    auto& singleDoc = dynamic_cast<DocumentSourceSingleDocumentTransformation&>(**iter);
    // DocumenSourceSingleDocumentTransformation reorders fields so we need to be insensitive.
    ASSERT(UnorderedFieldsBSONObjComparator{}.evaluate(
        BSON("_id" << true << "a"
                   << BSON("$convert" << BSON("input" << BSON("$const" << true) << "to"
                                                      << BSON("$const"
                                                              << "bool")))
                   << "b"
                   << BSON("$convert" << BSON("input" << BSON("$const" << 1.999999) << "to"
                                                      << BSON("$const"
                                                              << "int")
                                                      << "onError"
                                                      << BSON("$const"
                                                              << "Can't convert")
                                                      << "onNull" << BSON("$const" << 1)))) ==
        singleDoc.getTransformer().serializeTransformation(boost::none).toBson()));
}

TEST(CstPipelineTranslationTest, TranslatesConvertExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::convert,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"true"}}},
                                     {KeyFieldname::toArg, CNode{UserString{"bool"}}},
                                     {KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}},
                                     {KeyFieldname::onNullArg, CNode{UserInt{1}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson(
            "{$convert: {input: {$const: 'true'}, to: {$const: 'bool'}, onNull: {$const: 1}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToBoolExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserInt{0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 0}, to: {$const: 'bool'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDateExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserLong{0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 0}, to: {$const: 'date'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDecimalExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserDouble{2.02}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 2.02}, to: {$const: 'decimal'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToDoubleExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserString{"5.5"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: '5.5'}, to: {$const: 'double'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToIntExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserBoolean{true}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: true}, to: {$const: 'int'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToLongExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserDecimal{1.0}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: 1.0}, to: {$const: 'long'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToObjectIdExpression) {
    const auto cst = CNode{
        CNode::ObjectChildren{{KeyFieldname::toObjectId, CNode{UserFieldPath{"_id", false}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: '$_id', to: {$const: 'objectId'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesToStringExpression) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toString, CNode{UserBoolean{true}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: {$const: true}, to: {$const: 'string'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesTypeExpression) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::type, CNode{UserLong{1}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT(dynamic_cast<ExpressionType*>(expr.get()));
}


TEST(CstPipelineTranslationTest, AbsConstantTranslation) {
    auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserInt{-1}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [{$const: -1}]}")) ==
                                           expr->serialize(false)));
    cst = CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserDouble{-1.534}}}}};
    expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [{$const: -1.534}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, AbsVariableTransation) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::abs, CNode{UserFieldPath{"foo", false}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$abs: [\"$foo\"]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, CeilTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::ceil, CNode{UserDouble{1.578}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$ceil: [{$const: 1.578}]}")) ==
                                           expr->serialize(false)));
}
TEST(CstPipelineTranslationTest, DivideTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::divide,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5}}, CNode{UserDouble{1}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$divide: [{$const: 1.5}, {$const: 1}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, ExpTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::exponent, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$exp: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, FloorTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::floor, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$floor: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}
TEST(CstPipelineTranslationTest, LnTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::ln, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$ln: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, LogTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::ln,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5}}, CNode{UserDouble{10}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$ln: [{$const: 1.5}, {$const: 10}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, LogTenTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::logten, CNode{UserDouble{1.5}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$log10: [{$const: 1.5}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, ModTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::mod,
         CNode{CNode::ArrayChildren{CNode{UserDouble{15}}, CNode{UserDouble{10}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$mod: [{$const: 15}, {$const: 10}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, MultiplyTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::multiply,
         CNode{CNode::ArrayChildren{
             CNode{UserDouble{15}}, CNode{UserDouble{10}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$multiply: [{$const: 15}, {$const: 10}, {$const: 2}]}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, PowTranslationTest) {
    auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::pow,
         CNode{CNode::ArrayChildren{CNode{UserDouble{5}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$pow: [{$const: 5}, {$const: 2}]}")) ==
                                           expr->serialize(false)));
    cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::pow,
         CNode{CNode::ArrayChildren{CNode{UserDouble{5.846}}, CNode{UserDouble{2.846}}}}}}};
    expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$pow: [{$const: 5.846}, {$const: 2.846}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, RoundTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::round,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$round: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SqrtTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::sqrt, CNode{UserDouble{144}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$sqrt: [{$const: 144}]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, SubtractTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::subtract,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$subtract: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TruncTranslationTest) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::trunc,
         CNode{CNode::ArrayChildren{CNode{UserDouble{1.5786}}, CNode{UserDouble{2}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$trunc: [{$const: 1.5786}, {$const: 2}]}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesReplaceOneExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::replaceOne,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"Antonio"}}},
                                     {KeyFieldname::findArg, CNode{UserString{"Ant"}}},
                                     {KeyFieldname::replacementArg, CNode{UserString{"T"}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$replaceOne: {input: {$const: 'Antonio'}, find: {$const: 'Ant'}, "
                       "replacement: {$const: 'T'}}}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesReplaceAllExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::replaceAll,
         CNode{CNode::ObjectChildren{
             {KeyFieldname::inputArg, CNode{UserString{"10gen"}}},
             {KeyFieldname::findArg, CNode{UserString{"10gen"}}},
             {KeyFieldname::replacementArg, CNode{UserString{"MongoDB"}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$replaceAll: {input: {$const: '10gen'}, find: {$const: '10gen'}, "
                       "replacement: {$const: 'MongoDB'}}}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesTrimExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::trim,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"    10gen"}}},
                                     {KeyFieldname::charsArg, CNode{UserString{"ge"}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$trim: {input: {$const: '    10gen'}, chars: {$const: 'ge'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesTrimWithoutCharsExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::trim,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"    10gen "}}},
                                     {KeyFieldname::charsArg, CNode{KeyValue::absentKey}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$trim: {input: {$const: '    10gen '}}}")) == expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesLtrimExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::ltrim,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"    10gen"}}},
                                     {KeyFieldname::charsArg, CNode{UserString{"ge"}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$ltrim: {input: {$const: '    10gen'}, chars: {$const: 'ge'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesRtrimExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::rtrim,
         CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserString{"10gen "}}},
                                     {KeyFieldname::charsArg, CNode{UserString{"ge"}}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$rtrim: {input: {$const: '10gen '}, chars: {$const: 'ge'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesConcatExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::concat,
         CNode{CNode::ArrayChildren{
             CNode{UserString{"abc"}}, CNode{UserString{"def"}}, CNode{UserString{"1x5"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$concat: [{$const: 'abc'}, {$const: 'def'}, {$const: '1x5'}]}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, TranslatesDateToStringExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::dateToString,
         CNode{CNode::ObjectChildren{
             {KeyFieldname::dateArg, CNode{UserFieldPath{"date", false}}},
             {KeyFieldname::formatArg, CNode{UserString{"%Y-%m-%d"}}},
             {KeyFieldname::timezoneArg, CNode{UserString{"America/New_York"}}},
             {KeyFieldname::onNullArg, CNode{UserString{"8/10/20"}}},
         }}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(
            fromjson("{$dateToString: {date: \"$date\", format: {$const: \"%Y-%m-%d\"}, timezone: "
                     "{$const: \"America/New_York\"}, onNull: {$const: \"8/10/20\"}}}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesDateFromStringExpression) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::dateFromString,
         CNode{CNode::ObjectChildren{
             {KeyFieldname::dateStringArg, CNode{UserString{"'2017-02-08T12:10:40.787'"}}},
             {KeyFieldname::formatArg, CNode{KeyValue::absentKey}},
             {KeyFieldname::timezoneArg, CNode{UserString{"America/New_York"}}},
             {KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}},
             {KeyFieldname::onNullArg, CNode{KeyValue::absentKey}},
         }}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$dateFromString: {dateString: {$const: \"'2017-02-08T12:10:40.787'\"}, "
                       "format: {$const: \"America/New_York\"}}}")) == expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesIndexOfCP) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::indexOfCP,
         CNode{CNode::ArrayChildren{CNode{UserString{"ABC"}}, CNode{UserString{"B"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$indexOfCP: [{$const: \"ABC\"}, {$const: \"B\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesIndexOfBytes) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::indexOfBytes,
         CNode{CNode::ArrayChildren{CNode{UserString{"ABC"}}, CNode{UserString{"B"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$indexOfBytes: [{$const: \"ABC\"}, {$const: \"B\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesSplit) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::split,
         CNode{CNode::ArrayChildren{CNode{UserString{"sapalaiat"}}, CNode{UserString{"a"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$split: [{$const: \"sapalaiat\"}, {$const: \"a\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesStrLenBytes) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::strLenBytes, CNode{UserString{"four"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$strLenBytes: [{$const: \"four\"}]}")) == expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesStrLenCP) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::strLenCP, CNode{UserString{"four"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$strLenCP: [{$const: \"four\"}]}")) ==
                                           expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesStrCaseCmp) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::strcasecmp,
         CNode{CNode::ArrayChildren{CNode{UserString{"100"}}, CNode{UserString{"2"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$strcasecmp: [{$const: \"100\"}, {$const: \"2\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, DesugarsSubstrToSubstrBytes) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::substr,
         CNode{CNode::ArrayChildren{
             CNode{UserString{"abc"}}, CNode{UserInt{0}}, CNode{UserString{"a"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$substrBytes: [{$const: \"abc\"}, {$const: 0}, {$const: \"a\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesSubstrBytes) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::substrBytes,
         CNode{CNode::ArrayChildren{
             CNode{UserString{"abc"}}, CNode{UserInt{0}}, CNode{UserString{"a"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$substrBytes: [{$const: \"abc\"}, {$const: 0}, {$const: \"a\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesSubstrCP) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::substrCP,
         CNode{CNode::ArrayChildren{
             CNode{UserString{"abc"}}, CNode{UserInt{0}}, CNode{UserString{"a"}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$substrCP: [{$const: \"abc\"}, {$const: 0}, {$const: \"a\"}]}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesToLower) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toLower, CNode{UserString{"ABC"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$toLower: [{$const: \"ABC\"}]}")) ==
                                           expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesToUpper) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toUpper, CNode{UserString{"EZ as 123"}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$toUpper: [{$const: \"EZ as 123\"}]}")) == expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesRegexFind) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::regexFind,
                                     CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, CNode{UserString{"aeiou"}}},
                                         {KeyFieldname::regexArg, CNode{UserRegex{".*", "i"}}},
                                         {KeyFieldname::optionsArg, CNode{KeyValue::absentKey}},
                                     }}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$regexFind: {input: {$const: \"aeiou\"}, regex: {$const: /.*/i}}}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesRegexFindAll) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::regexFindAll,
                                     CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, CNode{UserString{"aeiou"}}},
                                         {KeyFieldname::regexArg, CNode{UserRegex{".*", "i"}}},
                                         {KeyFieldname::optionsArg, CNode{KeyValue::absentKey}},
                                     }}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$regexFindAll: {input: {$const: \"aeiou\"}, regex: {$const: /.*/i}}}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, TranslatesRegexMatch) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::regexMatch,
                                     CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, CNode{UserString{"aeiou"}}},
                                         {KeyFieldname::regexArg, CNode{UserRegex{".*", "i"}}},
                                         {KeyFieldname::optionsArg, CNode{KeyValue::absentKey}},
                                     }}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$regexMatch: {input: {$const: \"aeiou\"}, regex: {$const: /.*/i}}}")) ==
        expr->serialize(false)))
        << expr->serialize(false);
}

TEST(CstPipelineTranslationTest, RecognizesSingleDollarAsNonConst) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::trunc,
         CNode{CNode::ArrayChildren{CNode{UserFieldPath{"val", false}},
                                    CNode{UserFieldPath{"places", false}}}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(Value(fromjson("{$trunc: [\"$val\", \"$places\"]}")) ==
                                           expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, RecognizesDoubleDollarAsNonConst) {
    const auto cst =
        CNode{CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserFieldPath{"NOW", true}}}}};
    auto expr = cst_pipeline_translation::translateExpression(cst, getExpCtx());
    ASSERT_TRUE(ValueComparator().evaluate(
        Value(fromjson("{$convert: {input: \"$$NOW\", to: {$const: 'date'}}}")) ==
        expr->serialize(false)));
}

TEST(CstPipelineTranslationTest, InvalidDollarPrefixStringFails) {
    {
        const auto cst = CNode{
            CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserFieldPath{"NOWX", true}}}}};
        ASSERT_THROWS_CODE(cst_pipeline_translation::translateExpression(cst, getExpCtx()),
                           AssertionException,
                           17276);
    }
}

}  // namespace
}  // namespace mongo

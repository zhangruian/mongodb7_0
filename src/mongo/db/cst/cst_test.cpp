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

#include <string>

#include "mongo/bson/json.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/cst/pipeline_parser_gen.hpp"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstTest, BuildsAndPrints) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::atan2,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserDouble{2.0}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{atan2: [\"<UserDouble 3.000000>\", \"<UserDouble 2.000000>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::project,
             CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}},
                                         {KeyFieldname::id, CNode{KeyValue::falseKey}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{project : {a: \"<KeyValue trueKey>\", id: \"<KeyValue falseKey>\"}}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, EmptyPipeline) {
    CNode output;
    auto input = fromjson("{pipeline: []}");
    BSONLexer lexer(input["pipeline"].Array());
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_TRUE(stdx::get_if<CNode::ArrayChildren>(&output.payload));
    ASSERT_EQ(0, stdx::get_if<CNode::ArrayChildren>(&output.payload)->size());
}

TEST(CstGrammarTest, InvalidPipelineSpec) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{}]}");
        BSONLexer lexer(input["pipeline"].Array());
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(1, parseTree.parse());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$unknownStage: {}}]}");
        BSONLexer lexer(input["pipeline"].Array());
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(1, parseTree.parse());
    }
    {
        ASSERT_THROWS_CODE(
            [] {
                CNode output;
                auto input = fromjson("{pipeline: 'not an array'}");
                BSONLexer lexer(input["pipeline"].Array());
            }(),
            AssertionException,
            13111);
    }
}

TEST(CstGrammarTest, ParsesInternalInhibitOptimization) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: {}}]}");
        BSONLexer lexer(input["pipeline"].Array());
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::inhibitOptimization == stages[0].firstKeyFieldname());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: 'invalid'}]}");
        BSONLexer lexer(input["pipeline"].Array());
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(1, parseTree.parse());
    }
}

}  // namespace
}  // namespace mongo

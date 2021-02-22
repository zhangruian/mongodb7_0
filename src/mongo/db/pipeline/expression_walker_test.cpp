/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ExpressionWalkerTest : public AggregationContextFixture {
protected:
    auto jsonToPipeline(StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        NamespaceString testNss("test", "collection");
        auto command = AggregateCommand{testNss, rawPipeline};

        return Pipeline::parse(command.getPipeline(), getExpCtx());
    }

    auto parseExpression(std::string expressionString) {
        return Expression::parseExpression(
            getExpCtxRaw(), fromjson(expressionString), getExpCtx()->variablesParseState);
    }
};

using namespace std::string_literals;
using namespace expression_walker;

TEST_F(ExpressionWalkerTest, NothingTreeWalkSucceedsAndReturnsVoid) {
    struct {
        void postVisit(Expression*) {}
    } nothingWalker;
    auto expression = std::unique_ptr<Expression>{};
    static_assert(std::is_same_v<decltype(walk(&nothingWalker, expression.get())), void>);
    walk(&nothingWalker, expression.get());
}

TEST_F(ExpressionWalkerTest, PrintWalkReflectsMutation) {
    struct {
        void preVisit(Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "{$concat: [";
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression))
                string += "\""s + constant->getValue().getString() + "\"";
        }
        void inVisit(unsigned long long, Expression* expression) {
            string += ", ";
        }
        void postVisit(Expression* expression) {
            if (typeid(*expression) == typeid(ExpressionConcat))
                string += "]}";
        }

        std::string string;
    } stringWalker;

    auto expressionString = "{$concat: [\"black\", \"green\", \"yellow\"]}"s;
    auto expression = parseExpression(expressionString);
    walk(&stringWalker, expression.get());
    ASSERT_EQ(stringWalker.string, expressionString);

    struct {
        auto preVisit(Expression* expression) {
            if (auto constant = dynamic_cast<ExpressionConstant*>(expression))
                if (constant->getValue().getString() == "black")
                    return std::make_unique<ExpressionConstant>(expCtx, Value{"white"s});
            return std::unique_ptr<ExpressionConstant>{};
        }
        ExpressionContext* const expCtx;
    } whiteWalker{getExpCtxRaw()};

    ASSERT_FALSE(walk(&whiteWalker, expression.get()));
    stringWalker.string.clear();
    walk(&stringWalker, expression.get());
    ASSERT_EQ(stringWalker.string, "{$concat: [\"white\", \"green\", \"yellow\"]}"s);
}

TEST_F(ExpressionWalkerTest, RootNodeReplacable) {
    struct {
        auto postVisit(Expression* expression) {
            return std::make_unique<ExpressionConstant>(expCtx, Value{"soup"s});
        }
        ExpressionContext* const expCtx;
    } replaceWithSoup{getExpCtxRaw()};

    auto expressionString = "{$add: [2, 3, 4, {$atan2: [1, 0]}]}"s;
    auto expression = parseExpression(expressionString);
    auto resultExpression = walk(&replaceWithSoup, expression.get());
    ASSERT_VALUE_EQ(dynamic_cast<ExpressionConstant*>(resultExpression.get())->getValue(),
                    Value{"soup"s});
    // The input Expression, as a side effect, will have all its branches changed to soup by this
    // rewrite.
    for (auto&& child : dynamic_cast<ExpressionAdd*>(expression.get())->getChildren())
        ASSERT_VALUE_EQ(dynamic_cast<ExpressionConstant*>(child.get())->getValue(), Value{"soup"s});
}

TEST_F(ExpressionWalkerTest, InVisitCanCount) {
    struct {
        void inVisit(unsigned long long count, Expression*) {
            counter.push_back(count);
        }
        std::vector<unsigned long long> counter;
    } countWalker;

    auto expressionString = "{$and: [true, false, true, true, false, true]}"s;
    auto expression = parseExpression(expressionString);
    walk(&countWalker, expression.get());
    ASSERT(countWalker.counter == std::vector({1ull, 2ull, 3ull, 4ull, 5ull}));
}

}  // namespace
}  // namespace mongo

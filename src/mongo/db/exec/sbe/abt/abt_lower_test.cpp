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

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/exec/sbe"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
class ABTPlanGeneration : public unittest::Test {
protected:
    void runExpressionVariation(GoldenTestContext& gctx, const std::string& name, const ABT& n) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        auto expr = SBEExpressionLowering{env, map}.optimize(n);
        stream << expr->toString() << std::endl;
    }
};


TEST_F(ABTPlanGeneration, LowerConstantExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runExpressionVariation(ctx, "string", Constant::str("hello world"_sd));

    runExpressionVariation(ctx, "int64", Constant::int64(100));
    runExpressionVariation(ctx, "int32", Constant::int32(32));
    runExpressionVariation(ctx, "double", Constant::fromDouble(3.14));
    runExpressionVariation(ctx, "decimal", Constant::fromDecimal(Decimal128("3.14")));

    runExpressionVariation(ctx, "timestamp", Constant::timestamp(Timestamp::max()));
    runExpressionVariation(ctx, "date", Constant::date(Date_t::fromMillisSinceEpoch(100)));

    runExpressionVariation(ctx, "boolean true", Constant::boolean(true));
    runExpressionVariation(ctx, "boolean false", Constant::boolean(false));
}
}  // namespace
}  // namespace mongo::optimizer

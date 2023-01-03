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
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
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

    // SBE plans with scans print UUIDs. As there are no collections in these tests the UUIDS
    // are generated by the ScanStage. Remove them so they don't throw off the test output.
    std::string stripUUIDs(std::string str) {
        // UUIDs are printed with a leading '@' character, and in quotes.
        auto atIndex = str.find('@');
        // Expect a quote after the '@' in the plan.
        ASSERT_EQUALS('\"', str[atIndex + 1]);
        // The next character is a quote. Find the close quote.
        auto closeQuote = str.find('"', atIndex + 2);
        return str.substr(0, atIndex + 2) + "<collUUID>" + str.substr(closeQuote, str.length());
    }

    void runNodeVariation(GoldenTestContext& gctx,
                          const std::string& name,
                          const ABT& n,
                          NodeToGroupPropsMap& nodeMap) {
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
        boost::optional<sbe::value::SlotId> ridSlot;
        sbe::value::SlotIdGenerator ids;
        opt::unordered_map<std::string, ScanDefinition> scanDefs;
        scanDefs.insert({"collName", buildScanDefinition()});
        Metadata md(scanDefs);
        auto planStage = SBENodeLowering{env, map, ridSlot, ids, md, nodeMap, false}.optimize(n);
        sbe::DebugPrinter printer;
        stream << stripUUIDs(printer.print(*planStage)) << std::endl;
    }

    ScanDefinition buildScanDefinition() {
        ScanDefOptions opts;
        opts.insert({"type", "mongod"});
        opts.insert({"database", "test"});
        opts.insert({"uuid", UUID::gen().toString()});

        opt::unordered_map<std::string, IndexDefinition> indexDefs;
        MultikeynessTrie trie;
        DistributionAndPaths dnp(DistributionType::Centralized);
        bool exists = true;
        CEType ce{false};
        return ScanDefinition(opts, indexDefs, trie, dnp, exists, ce);
    }

    auto getNextNodeID() {
        return lastNodeGenerated++;
    }

    auto makeNodeProp() {
        NodeProps n{getNextNodeID(),
                    {},
                    {},
                    {},
                    boost::none,
                    CostType::fromDouble(0),
                    CostType::fromDouble(0),
                    {false}};
        n._planNodeId = getNextNodeID();
        return n;
    }
    void runPathLowering(ABT& tree) {
        auto env = VariableEnvironment::build(tree);
        auto prefixId = PrefixId::createForTests();
        runPathLowering(env, prefixId, tree);
    }
    void runPathLowering(VariableEnvironment& env, PrefixId& prefixId, ABT& tree) {
        // Run rewriters while things change
        bool changed = false;
        do {
            changed = false;
            if (PathLowering{prefixId, env}.optimize(tree)) {
                changed = true;
            }
            if (ConstEval{env}.optimize(tree)) {
                changed = true;
            }
        } while (changed);
    }

private:
    int32_t lastNodeGenerated = 0;
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

TEST_F(ABTPlanGeneration, LowerVarExpression) {
    NodeToGroupPropsMap nodeMap;
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    FieldProjectionMap map{{}, {ProjectionName{"scan0"}}, {}};
    ABT scanNode = make<PhysicalScanNode>(map, "collName", false);
    nodeMap.insert({scanNode.cast<PhysicalScanNode>(), makeNodeProp()});
    auto field = make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scan0"));
    runPathLowering(field);
    ABT evalNode = make<EvaluationNode>("proj0", std::move(field), std::move(scanNode));
    nodeMap.insert({evalNode.cast<EvaluationNode>(), makeNodeProp()});
    runNodeVariation(ctx, "varInProj", evalNode, nodeMap);
}
}  // namespace
}  // namespace mongo::optimizer

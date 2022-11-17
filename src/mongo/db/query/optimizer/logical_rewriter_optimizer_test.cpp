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

#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"


namespace mongo::optimizer {
namespace {

TEST(LogicalRewriter, RootNodeMerge) {
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("a", "test");
    ABT limitSkipNode1 =
        make<LimitSkipNode>(properties::LimitSkipRequirement(-1, 10), std::move(scanNode));
    ABT limitSkipNode2 =
        make<LimitSkipNode>(properties::LimitSkipRequirement(5, 0), std::move(limitSkipNode1));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a"}},
                                  std::move(limitSkipNode2));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    a\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "  LimitSkip []\n"
        "    limitSkip:\n"
        "      limit: 5\n"
        "      skip: 0\n"
        "    LimitSkip []\n"
        "      limitSkip:\n"
        "        limit: (none)\n"
        "        skip: 10\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [a]\n"
        "            Source []\n",
        rootNode);

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT rewritten = std::move(rootNode);
    phaseManager.optimize(rewritten);

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    a\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "  LimitSkip []\n"
        "    limitSkip:\n"
        "      limit: 5\n"
        "      skip: 10\n"
        "    Scan [test]\n"
        "      BindBlock:\n"
        "        [a]\n"
        "          Source []\n",
        rewritten);
}

TEST(LogicalRewriter, Memo) {
    using namespace cascades;
    using namespace properties;

    Metadata metadata{{{"test", {}}}};
    auto debugInfo = DebugInfo::kDefaultForTests;
    DefaultLogicalPropsDerivation lPropsDerivation;
    auto ceDerivation = makeHeuristicCE();
    Memo::Context memoCtx{&metadata, &debugInfo, &lPropsDerivation, ceDerivation.get()};
    Memo memo;

    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathConstant>(make<UnaryOp>(Operations::Neg, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT evalNode = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(filterNode));

    NodeIdSet insertedNodeIds;
    const GroupIdType rootGroupId = memo.integrate(memoCtx, evalNode, {}, insertedNodeIds);
    ASSERT_EQ(2, rootGroupId);
    ASSERT_EQ(3, memo.getGroupCount());

    NodeIdSet expectedInsertedNodeIds = {{0, 0}, {1, 0}, {2, 0}};
    ASSERT_TRUE(insertedNodeIds == expectedInsertedNodeIds);

    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 100\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Filter []\n"
        "    |           |   EvalFilter []\n"
        "    |           |   |   Variable [ptest]\n"
        "    |           |   PathConstant []\n"
        "    |           |   UnaryOp [Neg]\n"
        "    |           |   Const [1]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 100\n"
        "    |   |       projections: \n"
        "    |   |           P1\n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Evaluation []\n"
        "    |           |   BindBlock:\n"
        "    |           |       [P1]\n"
        "    |           |           EvalPath []\n"
        "    |           |           |   Variable [ptest]\n"
        "    |           |           PathConstant []\n"
        "    |           |           Const [2]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n",
        memo);

    {
        // Try to insert into the memo again.
        NodeIdSet insertedNodeIds;
        const GroupIdType group = memo.integrate(memoCtx, evalNode, {}, insertedNodeIds);
        ASSERT_EQ(2, group);
        ASSERT_EQ(3, memo.getGroupCount());

        // Nothing was inserted.
        ASSERT_EQ(1, memo.getLogicalNodes(0).size());
        ASSERT_EQ(1, memo.getLogicalNodes(1).size());
        ASSERT_EQ(1, memo.getLogicalNodes(2).size());
    }

    // Insert a different tree, this time only scan and project.
    ABT scanNode1 = make<ScanNode>("ptest", "test");
    ABT evalNode1 = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(scanNode1));

    {
        NodeIdSet insertedNodeIds1;
        const GroupIdType rootGroupId1 = memo.integrate(memoCtx, evalNode1, {}, insertedNodeIds1);
        ASSERT_EQ(3, rootGroupId1);
        ASSERT_EQ(4, memo.getGroupCount());

        // Nothing was inserted in first 3 groups.
        ASSERT_EQ(1, memo.getLogicalNodes(0).size());
        ASSERT_EQ(1, memo.getLogicalNodes(1).size());
        ASSERT_EQ(1, memo.getLogicalNodes(2).size());
    }

    {
        ASSERT_EQ(1, memo.getLogicalNodes(3).size());

        ASSERT_EXPLAIN(
            "Evaluation []\n"
            "  BindBlock:\n"
            "    [P1]\n"
            "      EvalPath []\n"
            "        PathConstant []\n"
            "          Const [2]\n"
            "        Variable [ptest]\n"
            "  MemoLogicalDelegator [groupId: 0]\n",
            memo.getLogicalNodes(3).front());
    }
}

TEST(LogicalRewriter, FilterProjectRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"ptest", CollationOp::Ascending}}), std::move(scanNode));
    ABT evalNode =
        make<EvaluationNode>("P1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(collationNode));
    ABT filterNode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("P1")),
                                      std::move(evalNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{{}}, std::move(filterNode));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "  RefBlock: \n"
        "  Filter []\n"
        "    EvalFilter []\n"
        "      PathIdentity []\n"
        "      Variable [P1]\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [P1]\n"
        "          EvalPath []\n"
        "            PathIdentity []\n"
        "            Variable [ptest]\n"
        "      Collation []\n"
        "        collation: \n"
        "          ptest: Ascending\n"
        "        RefBlock: \n"
        "          Variable [ptest]\n"
        "        Scan [test]\n"
        "          BindBlock:\n"
        "            [ptest]\n"
        "              Source []\n",
        rootNode);

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "  RefBlock: \n"
        "  Collation []\n"
        "    collation: \n"
        "      ptest: Ascending\n"
        "    RefBlock: \n"
        "      Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathIdentity []\n"
        "        Variable [P1]\n"
        "      Evaluation []\n"
        "        BindBlock:\n"
        "          [P1]\n"
        "            EvalPath []\n"
        "              PathIdentity []\n"
        "              Variable [ptest]\n"
        "        Scan [test]\n"
        "          BindBlock:\n"
        "            [ptest]\n"
        "              Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectComplexRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projection2Node = make<EvaluationNode>(
        "p2", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));

    ABT projection3Node =
        make<EvaluationNode>("p3",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projection2Node));

    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"ptest", CollationOp::Ascending}}), std::move(projection3Node));

    ABT projection1Node =
        make<EvaluationNode>("p1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(collationNode));

    ABT filter1Node = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("p1")),
                                       std::move(projection1Node));

    ABT filterScanNode = make<FilterNode>(
        make<EvalFilter>(make<PathIdentity>(), make<Variable>("ptest")), std::move(filter1Node));

    ABT filter2Node = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("p2")),
                                       std::move(filterScanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{{}}, std::move(filter2Node));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   RefBlock: \n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       ptest: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p3]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        rootNode);

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // Note: this assert depends on the order on which we consider rewrites.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   RefBlock: \n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       ptest: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p3]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectGroupRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"a"},
                                        ProjectionNameVector{"c"},
                                        makeSeq(make<Variable>("b")),
                                        std::move(projectionBNode));

    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(groupByNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"c"}},
                                  std::move(filterANode));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       c\n"
        "|   RefBlock: \n"
        "|       Variable [c]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [a]\n"
        "|   aggregations: \n"
        "|       [c]\n"
        "|           Variable [b]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectUnwindRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT unwindNode =
        make<UnwindNode>("a", "a_pid", false /*retainNonArrays*/, std::move(projectionBNode));

    // This filter should stay above the unwind.
    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(unwindNode));

    // This filter should be pushed down below the unwind.
    ABT filterBNode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("b")),
                                       std::move(filterANode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(filterBNode));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           Source []\n"
        "|       [a_pid]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectExchangeRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT exchangeNode = make<ExchangeNode>(
        properties::DistributionRequirement({DistributionType::HashPartitioning, {"a"}}),
        std::move(projectionBNode));

    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(exchangeNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(filterANode));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: HashPartitioning\n"
        "|   |           projections: \n"
        "|   |               a\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, UnwindCollationRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    // This collation node should stay below the unwind.
    ABT collationANode = make<CollationNode>(CollationRequirement({{"a", CollationOp::Ascending}}),
                                             std::move(projectionBNode));

    // This collation node should go above the unwind.
    ABT collationBNode = make<CollationNode>(CollationRequirement({{"b", CollationOp::Ascending}}),
                                             std::move(collationANode));

    ABT unwindNode =
        make<UnwindNode>("a", "a_pid", false /*retainNonArrays*/, std::move(collationBNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(unwindNode));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       b: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [b]\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           Source []\n"
        "|       [a_pid]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterUnionReorderSingleProjection) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");
    // Create two eval nodes such that the two branches of the union share a projection.
    ABT evalNode1 =
        make<EvaluationNode>("pUnion",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(scanNode1));
    ABT evalNode2 =
        make<EvaluationNode>("pUnion",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(scanNode2));

    ABT unionNode = make<UnionNode>(ProjectionNameVector{"pUnion"}, makeSeq(evalNode1, evalNode2));

    ABT filter = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion")),
        std::move(unionNode));
    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"pUnion"}},
                                  std::move(filter));

    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);

    auto phaseManager =
        makePhaseManager({OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
                         prefixId,
                         {{{"test1", createScanDef({}, {})}, {"test2", createScanDef({}, {})}}},
                         DebugInfo::kDefaultForTests);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           Source []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, MultipleFilterUnionReorder) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");

    // Create multiple shared projections for each child.
    ABT pUnion11 =
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(scanNode1));
    ABT pUnion12 =
        make<EvaluationNode>("pUnion2",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(pUnion11));

    ABT pUnion21 =
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(scanNode2));
    ABT pUnion22 =
        make<EvaluationNode>("pUnion2",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(pUnion21));

    ABT unionNode =
        make<UnionNode>(ProjectionNameVector{"pUnion1", "pUnion2"}, makeSeq(pUnion12, pUnion22));

    // Create two filters, one for each of the two common projections.
    ABT filterUnion1 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion1")),
        std::move(unionNode));
    ABT filterUnion2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion2")),
        std::move(filterUnion1));
    ABT rootNode = make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{"pUnion1", "pUnion2"}},
        std::move(filterUnion2));

    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion1\n"
        "|   |       pUnion2\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion1]\n"
        "|       Variable [pUnion2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion2]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion1]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           Source []\n"
        "|   |       [pUnion2]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion2]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);

    auto phaseManager =
        makePhaseManager({OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
                         prefixId,
                         {{{"test1", createScanDef({}, {})}, {"test2", createScanDef({}, {})}}},
                         DebugInfo::kDefaultForTests);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion1\n"
        "|   |       pUnion2\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion1]\n"
        "|       Variable [pUnion2]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           Source []\n"
        "|   |       [pUnion2]\n"
        "|   |           Source []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion2]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion2]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion1]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion2]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion1]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterUnionUnionPushdown) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest", "test1");
    ABT scanNode2 = make<ScanNode>("ptest", "test2");
    ABT unionNode = make<UnionNode>(ProjectionNameVector{"ptest"}, makeSeq(scanNode1, scanNode2));

    ABT scanNode3 = make<ScanNode>("ptest", "test3");
    ABT parentUnionNode =
        make<UnionNode>(ProjectionNameVector{"ptest"}, makeSeq(unionNode, scanNode3));

    ABT filter = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("ptest")),
        std::move(parentUnionNode));
    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                                  std::move(filter));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test1", createScanDef({}, {})},
                                           {"test2", createScanDef({}, {})},
                                           {"test3", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Scan [test3]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);

    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Sargable [Complete]\n"
        "|   |   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{=Const [1]}}}\n"
        "|   |   |   |   |   candidateIndexes: \n"
        "|   |   |   |   scanParams: \n"
        "|   |   |   |       {'a': evalTemp_0}\n"
        "|   |   |   |           residualReqs: \n"
        "|   |   |   |               refProjection: evalTemp_0, path: 'PathTraverse [1] "
        "PathIdentity []', intervals: {{{=Const [1]}}}, entryIndex: 0\n"
        "|   |   |   BindBlock:\n"
        "|   |   RefBlock: \n"
        "|   |       Variable [ptest]\n"
        "|   Scan [test3]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Sargable [Complete]\n"
        "|   |   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{=Const [1]}}}\n"
        "|   |   |   |   |   candidateIndexes: \n"
        "|   |   |   |   scanParams: \n"
        "|   |   |   |       {'a': evalTemp_2}\n"
        "|   |   |   |           residualReqs: \n"
        "|   |   |   |               refProjection: evalTemp_2, path: 'PathTraverse [1] "
        "PathIdentity []', intervals: {{{=Const [1]}}}, entryIndex: 0\n"
        "|   |   |   BindBlock:\n"
        "|   |   RefBlock: \n"
        "|   |       Variable [ptest]\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{=Const [1]}}}\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {'a': evalTemp_1}\n"
        "|   |   |           residualReqs: \n"
        "|   |   |               refProjection: evalTemp_1, path: 'PathTraverse [1] PathIdentity "
        "[]', intervals: {{{=Const [1]}}}, entryIndex: 0\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, UnionPreservesCommonLogicalProps) {
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");
    ABT evalNode1 = make<EvaluationNode>(
        "a",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(scanNode1));

    ABT evalNode2 = make<EvaluationNode>(
        "a",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(scanNode2));
    ABT unionNode = make<UnionNode>(ProjectionNameVector{"a"}, makeSeq(evalNode1, evalNode2));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a"}},
                                  std::move(unionNode));

    Metadata metadata{{{"test1",
                        createScanDef({},
                                      {},
                                      ConstEval::constFold,
                                      {DistributionType::HashPartitioning,
                                       makeSeq(make<PathGet>("a", make<PathIdentity>()))})},
                       {"test2",
                        createScanDef({},
                                      {},
                                      ConstEval::constFold,
                                      {DistributionType::HashPartitioning,
                                       makeSeq(make<PathGet>("a", make<PathIdentity>()))})}},
                      2};

    // Run the reordering rewrite such that the scan produces a hash partition.
    PrefixId prefixId;
    auto phaseManager =
        makePhaseManager({OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
                         prefixId,
                         metadata,
                         DebugInfo::kDefaultForTests);

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest1\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest1, scanDefName: test1, "
        "eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test1]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest1]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest1, path: 'PathGet [a] PathIdentity []', ce: "
        "1000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |           ptest1\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest1, scanDefName: test1]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest1, path: 'PathGet [a] "
        "PathIdentity []', boundProjection: a, intervals: {{{<fully open>}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': a}\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest1]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest2\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 2, scanProjection: ptest2, scanDefName: test2, "
        "eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test2]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest2]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 3\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest2, path: 'PathGet [a] PathIdentity []', ce: "
        "1000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |           ptest2\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 2, scanProjection: ptest2, scanDefName: test2]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest2, path: 'PathGet [a] "
        "PathIdentity []', boundProjection: a, intervals: {{{<fully open>}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': a}\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest2]\n"
        "    |           MemoLogicalDelegator [groupId: 2]\n"
        "    physicalNodes: \n"
        "    groupId: 4\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 2000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Union []\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   MemoLogicalDelegator [groupId: 3]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n"
        "    groupId: 5\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 2000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Root []\n"
        "    |           |   |   projections: \n"
        "    |           |   |       a\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [a]\n"
        "    |           MemoLogicalDelegator [groupId: 4]\n"
        "    physicalNodes: \n",
        phaseManager.getMemo());
}

ABT sargableCETestSetup() {
    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b", make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("ptest")),
        std::move(filterANode));

    return make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                          std::move(filterBNode));
}

TEST(LogicalRewriter, SargableCE) {
    using namespace properties;

    PrefixId prefixId;
    ABT rootNode = sargableCETestSetup();
    auto phaseManager =
        makePhaseManager({OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
                         prefixId,
                         {{{"test", createScanDef({}, {})}}},
                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // Displays SargableNode-specific per-key estimates.
    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 5.62341\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest, path: 'PathGet [a] PathIdentity []', ce: "
        "31.6228\n"
        "    |   |               refProjection: ptest, path: 'PathGet [b] PathIdentity []', ce: "
        "31.6228\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly, "
        "hasProperInterval]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] "
        "PathIdentity []', intervals: {{{=Const [1]}}}\n"
        "    |           |   |   |   |   |       refProjection: ptest, path: 'PathGet [b] "
        "PathIdentity []', intervals: {{{=Const [2]}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': evalTemp_2, 'b': evalTemp_3}\n"
        "    |           |   |   |           residualReqs: \n"
        "    |           |   |   |               refProjection: evalTemp_2, path: 'PathIdentity "
        "[]', intervals: {{{=Const [1]}}}, entryIndex: 0\n"
        "    |           |   |   |               refProjection: evalTemp_3, path: 'PathIdentity "
        "[]', intervals: {{{=Const [2]}}}, entryIndex: 1\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 5.62341\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly, "
        "hasProperInterval]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Root []\n"
        "    |           |   |   projections: \n"
        "    |           |   |       ptest\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n",
        phaseManager.getMemo());
}

TEST(LogicalRewriter, RemoveNoopFilter) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Gte, Constant::minKey())),
                         make<Variable>("ptest")),
        std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                                  std::move(filterANode));

    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, NotPushdownToplevel) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("scan_0", "coll");

    ABT abEq3 = make<PathGet>(
        "a",
        make<PathTraverse>(
            make<PathGet>("b",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(3)),
                                             PathTraverse::kSingleLevel)),
            PathTraverse::kSingleLevel));
    ABT filterNode = make<FilterNode>(
        make<UnaryOp>(Operations::Not, make<EvalFilter>(abEq3, make<Variable>("scan_0"))),
        std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre, OptPhase::MemoSubstitutionPhase},
        prefixId,
        Metadata{{
            {"coll",
             createScanDef(
                 {},
                 {{"index1",
                   IndexDefinition{// collation
                                   {{makeIndexPath(FieldPathType{"a", "b"}, false /*isMultiKey*/),
                                     CollationOp::Ascending}},
                                   false /*isMultiKey*/,
                                   {DistributionType::Centralized},
                                   {} /*partialReqMap*/}}})},
        }},
        DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // TODO SERVER-70224 We remove the Traverse nodes, and combine the Not ... Eq into Neq.
    // For now we only remove Traverse nodes.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   UnaryOp [Not]\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [3]\n"
        "Scan [coll]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, NotPushdownUnderLambdaSuccess) {
    // Example translation of {a: {$elemMatch: {b: {$ne: 2}}}}
    ABT scanNode = make<ScanNode>("scan_0", "coll");
    ABT path = make<PathGet>(
        "a",
        make<PathComposeM>(
            make<PathArr>(),
            make<PathTraverse>(
                make<PathComposeM>(
                    make<PathComposeA>(make<PathArr>(), make<PathObj>()),
                    make<PathLambda>(make<LambdaAbstraction>(
                        "match_0_not_0",
                        make<UnaryOp>(Operations::Not,
                                      make<EvalFilter>(
                                          make<PathGet>("b",
                                                        make<PathTraverse>(
                                                            make<PathCompare>(Operations::Eq,
                                                                              Constant::int64(2)),
                                                            PathTraverse::kSingleLevel)),
                                          make<Variable>("match_0_not_0")))))),
                PathTraverse::kSingleLevel)));
    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(path, make<Variable>("scan_0")), std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    PrefixId prefixId;
    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre, OptPhase::MemoSubstitutionPhase},
        prefixId,
        Metadata{{
            {"coll",
             createScanDef(
                 {},
                 {{"index1",
                   IndexDefinition{// collation
                                   {{makeIndexPath(FieldPathType{"a", "b"}, false /*isMultiKey*/),
                                     CollationOp::Ascending}},
                                   false /*isMultiKey*/,
                                   {DistributionType::Centralized},
                                   {} /*partialReqMap*/}}})},
        }},
        DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // TODO SERVER-70224 All the Traverses should be eliminated, and the Not ... Eq combined as Neq.
    // For now we only remove the Traverse nodes.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathLambda []\n"
        "|   LambdaAbstraction [match_0_not_0]\n"
        "|   UnaryOp [Not]\n"
        "|   EvalFilter []\n"
        "|   |   Variable [match_0_not_0]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathIdentity []', inte"
        "rvals: {{{[Const [[]], Const [BinData(0, )])}}}\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {'a': evalTemp_2}\n"
        "|   |   |           residualReqs: \n"
        "|   |   |               refProjection: evalTemp_2, path: 'PathIdentity []', intervals: {"
        "{{[Const [[]], Const [BinData(0, )])}}}, entryIndex: 0\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Scan [coll]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, NotPushdownUnderLambdaKeepOuterTraverse) {
    // Like 'NotPushdownUnderLambdaSuccess', but 'a' is multikey,
    // so we can only remove the inner traverse, at 'a.b'.
    ABT scanNode = make<ScanNode>("scan_0", "coll");
    ABT path = make<PathGet>(
        "a",
        make<PathComposeM>(
            make<PathArr>(),
            make<PathTraverse>(
                make<PathComposeM>(
                    make<PathComposeA>(make<PathArr>(), make<PathObj>()),
                    make<PathLambda>(make<LambdaAbstraction>(
                        "match_0_not_0",
                        make<UnaryOp>(Operations::Not,
                                      make<EvalFilter>(
                                          make<PathGet>("b",
                                                        make<PathTraverse>(
                                                            make<PathCompare>(Operations::Eq,
                                                                              Constant::int64(2)),
                                                            PathTraverse::kSingleLevel)),
                                          make<Variable>("match_0_not_0")))))),
                PathTraverse::kSingleLevel)));
    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(path, make<Variable>("scan_0")), std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    PrefixId prefixId;
    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre, OptPhase::MemoSubstitutionPhase},
        prefixId,
        Metadata{{
            {"coll",
             createScanDef(
                 {},
                 {{"index1",
                   IndexDefinition{
                       // collation
                       {{make<PathGet>("a",
                                       make<PathTraverse>(make<PathGet>("b", make<PathIdentity>()),
                                                          PathTraverse::kSingleLevel)),
                         CollationOp::Ascending}},
                       false /*isMultiKey*/,
                       {DistributionType::Centralized},
                       {} /*partialReqMap*/}}})},
        }},
        DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // TODO SERVER-70224 The inner Traverses should be eliminated, and the Not ... Eq combined as
    // Neq. We have to keep the outer traverse since 'a' is multikey. (Until SERVER-70224, we only
    // remove Traverse nodes.)
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathLambda []\n"
        "|   |   LambdaAbstraction [match_0_not_0]\n"
        "|   |   UnaryOp [Not]\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [match_0_not_0]\n"
        "|   |   PathGet [b]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [2]\n"
        "|   PathComposeA []\n"
        "|   |   PathObj []\n"
        "|   PathArr []\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathIdentity []', inte"
        "rvals: {{{[Const [[]], Const [BinData(0, )])}}}\n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathTraverse [1] PathI"
        "dentity []', intervals: {{{[Const [{}], Const [[]])}} U {{[Const [[]], Const [BinData(0,"
        " )])}}}, perfOnly\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {'a': evalTemp_1}\n"
        "|   |   |           residualReqs: \n"
        "|   |   |               refProjection: evalTemp_1, path: 'PathIdentity []', intervals: {"
        "{{[Const [[]], Const [BinData(0, )])}}}, entryIndex: 0\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Scan [coll]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, RemoveTraverseSplitComposeM) {
    // When we have a filter with Traverse above ComposeM, we can't immediately
    // split the ComposeM into a top-level conjunction.  But if we can use multikeyness
    // to remove the Traverse first, then we can split it.

    // This query is similar to $elemMatch, but without the PathArr constraint.
    ABT scanNode = make<ScanNode>("scan_0", "coll");
    ABT path = make<PathGet>(
        "a",
        make<PathTraverse>(
            make<PathGet>(
                "b",
                make<PathTraverse>(
                    make<PathComposeM>(make<PathCompare>(Operations::Gt, Constant::int64(3)),
                                       make<PathCompare>(Operations::Lt, Constant::int64(8))),
                    PathTraverse::kSingleLevel)),
            PathTraverse::kSingleLevel));
    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(path, make<Variable>("scan_0")), std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    PrefixId prefixId;
    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre, OptPhase::MemoSubstitutionPhase},
        prefixId,
        Metadata{{
            {"coll",
             createScanDef(
                 {},
                 {{"index1",
                   IndexDefinition{// collation
                                   {{makeIndexPath(FieldPathType{"a", "b"}, false /*isMultiKey*/),
                                     CollationOp::Ascending}},
                                   false /*isMultiKey*/,
                                   {DistributionType::Centralized},
                                   {} /*partialReqMap*/}}})},
        }},
        DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // We should end up with a Sargable node and no residual Filter.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathGet [b] "
        "PathIdentity []', intervals: {{{(Const [3], Const [8])}}}\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   |       candidateId: 1, index1, {}, {0}, {{{(Const [3], Const [8])}}}\n"
        "|   |   |   scanParams: \n"
        "|   |   |       {'a': evalTemp_2}\n"
        "|   |   |           residualReqs: \n"
        "|   |   |               refProjection: evalTemp_2, path: 'PathGet [b] PathIdentity []', "
        "intervals: {{{(Const [3], Const [8])}}}, entryIndex: 0\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Scan [coll]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, TraverseComposeMTraverse) {
    // When we have a filter with Get a (Traverse (ComposeM _ (Traverse ...))), we should not
    // simplify under the inner Traverse, because MultikeynessTrie contains no information about
    // doubly-nested arrays.

    ABT scanNode = make<ScanNode>("scan_0", "coll");
    ABT path = make<PathGet>(
        "a",
        make<PathTraverse>(
            make<PathComposeM>(
                make<PathComposeA>(make<PathArr>(), make<PathObj>()),
                make<PathTraverse>(
                    make<PathGet>(
                        "b",
                        make<PathTraverse>(make<PathCompare>(Operations::Gt, Constant::int64(3)),
                                           PathTraverse::kSingleLevel)),
                    PathTraverse::kSingleLevel)),
            PathTraverse::kSingleLevel));

    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(path, make<Variable>("scan_0")), std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    PrefixId prefixId;
    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre, OptPhase::MemoSubstitutionPhase},
        prefixId,
        Metadata{{
            {"coll",
             createScanDef({},
                           {{"index1",
                             IndexDefinition{
                                 // collation
                                 {{make<PathGet>("a",
                                                 make<PathTraverse>(
                                                     // 'a' is multikey, but 'a.b' is non-multikey.
                                                     make<PathGet>("b", make<PathIdentity>()),
                                                     PathTraverse::kSingleLevel)),
                                   CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {} /*partialReqMap*/}}})},
        }},
        DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    phaseManager.optimize(latest);

    // The resulting Filter node should keep all the Traverse nodes:
    // - Keep the outermost two because 'a' is multikey.
    // - Keep the innermost because we don't know anything about the contents
    //   of doubly-nested arrays.
    // (We may also get a perfOnly Sargable node; that's not the point of this test.)
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathGet [b]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [3]\n"
        "|   PathComposeA []\n"
        "|   |   PathObj []\n"
        "|   PathArr []\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathTraverse [1] PathI"
        "dentity []', intervals: {{{[Const [{}], Const [[]])}} U {{[Const [[]], Const [BinData(0,"
        " )])}}}, perfOnly\n"
        "|   |   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathTraverse [1] PathT"
        "raverse [1] PathGet [b] PathTraverse [1] PathIdentity []', intervals: {{{>Const [3]}}}, "
        "perfOnly\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {}\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Scan [coll]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, RelaxComposeM) {
    // When we have a ComposeM that:
    // - cannot be split into a top-level conjunction, and
    // - has a sargable predicate on only one side
    // then we generate a Sargable node with a perfOnly predicate.

    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT path = make<PathGet>(
        "a",
        make<PathTraverse>(
            make<PathComposeM>(
                // One side is sargable.
                make<PathGet>("b", make<PathCompare>(Operations::Gt, Constant::int64(0))),
                // One side is not sargable.
                // A common example is Traverse inside Not: we can't push Not
                // to the leaf because Traverse is a disjunction (over array elements).
                make<PathLambda>(make<LambdaAbstraction>(
                    "x",
                    make<UnaryOp>(
                        Operations::Not,
                        make<EvalFilter>(
                            make<PathGet>("b",
                                          make<PathTraverse>(
                                              make<PathCompare>(Operations::Eq, Constant::int64(3)),
                                              PathTraverse::kSingleLevel)),
                            make<Variable>("x")))))),
            PathTraverse::kSingleLevel));

    ABT filterNode = make<FilterNode>(make<EvalFilter>(std::move(path), make<Variable>("root")),
                                      std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    PrefixId prefixId;
    auto phaseManager = makePhaseManager({OptPhase::MemoSubstitutionPhase},
                                         prefixId,
                                         {{{"c1", createScanDef({}, {})}}},
                                         DebugInfo::kDefaultForTests,
                                         {} /*hints*/);

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       root\n"
        "|   RefBlock: \n"
        "|       Variable [root]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathLambda []\n"
        "|   |   LambdaAbstraction [x]\n"
        "|   |   UnaryOp [Not]\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [x]\n"
        "|   |   PathGet [b]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [3]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [0]\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: root, path: 'PathGet [a] PathTraverse [1] PathGet"
        " [b] PathIdentity []', intervals: {{{>Const [0]}}}, perfOnly\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {}\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [root]\n"
        "Scan [c1]\n"
        "    BindBlock:\n"
        "        [root]\n"
        "            Source []\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingRIN) {
    using namespace properties;
    using namespace unit_test_abt_literals;
    PrefixId prefixId;

    // Construct a query which tests "a" = 1 and "b" = 2 and "c" = 3.
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_get("e", _traverse1(_cmp("Eq", "3"_cint64))), "root"_var))
                       .filter(_evalf(_get("c", _traverse1(_cmp("Eq", "2"_cint64))), "root"_var))
                       .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                       .finish(_scan("root", "c1"));

    // We have one index with 5 fields: "a", "b", "c", "d", "e".
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("d"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("e"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.getHints()._maxIndexEqPrefixes = 3;
    phaseManager.optimize(optimized);
    // No plans explored: testing only substitution phase.
    ASSERT_EQ(0, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // The resulting sargable node is too big to explain in its entirety. We explain the important
    // pieces.
    const SargableNode& node = *optimized.cast<RootNode>()->getChild().cast<SargableNode>();

    // Demonstrate we encode intervals for "a", "c", and "e".
    ASSERT_EQ(
        "requirementsMap: \n"
        "    refProjection: root, path: 'PathGet [a] PathIdentity []', intervals: {{{=Const "
        "[1]}}}\n"
        "    refProjection: root, path: 'PathGet [c] PathIdentity []', intervals: {{{=Const "
        "[2]}}}\n"
        "    refProjection: root, path: 'PathGet [e] PathIdentity []', intervals: {{{=Const "
        "[3]}}}\n",
        ExplainGenerator::explainPartialSchemaReqMap(node.getReqMap()));

    const auto& ci = node.getCandidateIndexes();

    ASSERT_EQ(3, ci.size());

    // We have one equality prefix for the first candidate index.
    ASSERT_EQ(1, ci.at(0)._intervals.size());

    // The first index field ("a") is constrained to 1, the remaining fields are not constrained.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Const [1], <fully open>, <fully open>, <fully open>, <fully open>}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(0)._intervals.front()));

    // We have two residual predicates for "c" and "e".
    ASSERT_EQ(
        "residualReqs: \n"
        "    refProjection: evalTemp_24, path: 'PathIdentity []', intervals: {{{=Const [2]}}}, "
        "entryIndex: 1\n"
        "    refProjection: evalTemp_25, path: 'PathIdentity []', intervals: {{{=Const [3]}}}, "
        "entryIndex: 2\n",
        ExplainGenerator::explainResidualRequirements(ci.at(0)._residualRequirements));


    // The second candidate index has two equality prefixes.
    ASSERT_EQ(2, ci.at(1)._intervals.size());

    // The first index field ("a") is again constrained to 1, and the remaining ones are not.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Const [1], <fully open>, <fully open>, <fully open>, <fully open>}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(1)._intervals.at(0)));

    // The first two index fields are constrained to variables obtained from the first scan, the
    // third one ("c") is bound to "2". The last two fields are unconstrained.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Variable [evalTemp_26], =Variable [evalTemp_27], =Const [2], <fully open>, "
        "<fully open>}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(1)._intervals.at(1)));

    // We have only one residual predicates for "e".
    ASSERT_EQ(
        "residualReqs: \n"
        "    refProjection: evalTemp_28, path: 'PathIdentity []', intervals: {{{=Const [3]}}}, "
        "entryIndex: 2\n",
        ExplainGenerator::explainResidualRequirements(ci.at(1)._residualRequirements));


    // The third candidate index has three equality prefixes.
    ASSERT_EQ(3, ci.at(2)._intervals.size());

    // The first index field ("a") is again constrained to 1.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Const [1], <fully open>, <fully open>, <fully open>, <fully open>}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(2)._intervals.at(0)));

    // The first two index fields are constrained to variables obtained from the first scan, the
    // third one ("c") is bound to "2". The last two fields are unconstrained.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Variable [evalTemp_29], =Variable [evalTemp_30], =Const [2], <fully open>, "
        "<fully open>}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(2)._intervals.at(1)));

    // The first 4 index fields are constrained to variables from the second scan, and the last one
    // to 4.
    ASSERT_EQ(
        "{\n"
        "    {\n"
        "        {=Variable [evalTemp_29], =Variable [evalTemp_30], =Variable [evalTemp_31], "
        "=Variable [evalTemp_32], =Const [3]}\n"
        "    }\n"
        "}\n",
        ExplainGenerator::explainIntervalExpr(ci.at(2)._intervals.at(2)));
}

}  // namespace
}  // namespace mongo::optimizer

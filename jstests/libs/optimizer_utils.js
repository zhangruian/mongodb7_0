load("jstests/libs/analyze_plan.js");

/*
 * Utility for checking if the query optimizer is enabled.
 */
function checkCascadesOptimizerEnabled(theDB) {
    const param = theDB.adminCommand({getParameter: 1, featureFlagCommonQueryFramework: 1});
    return param.hasOwnProperty("featureFlagCommonQueryFramework") &&
        param.featureFlagCommonQueryFramework.value;
}

/**
 * Given the result of an explain command, returns whether the bonsai optimizer was used.
 */
function usedBonsaiOptimizer(explain) {
    if (explain.hasOwnProperty("queryPlanner") &&
        !explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan")) {
        // Find command explain which means new optimizer was not used.
        // TODO SERVER-62407 this assumption may no longer hold true if the translation to ABT
        // happens directly from a find command.
        return false;
    }

    const plannerOutput = getAggPlanStage(explain, "$cursor");
    if (plannerOutput != null) {
        return plannerOutput["$cursor"].queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    } else {
        return explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    }
}

/**
 * Given a query plan or explain output, follow the leftmost child until
 * we reach a leaf stage, and return it.
 *
 * This is useful for finding the access path part of a plan, typically a PhysicalScan or IndexScan.
 */
function leftmostLeafStage(node) {
    for (;;) {
        if (node.queryPlanner) {
            node = node.queryPlanner;
        } else if (node.winningPlan) {
            node = node.winningPlan;
        } else if (node.optimizerPlan) {
            node = node.optimizerPlan;
        } else if (node.child) {
            node = node.child;
        } else if (node.leftChild) {
            node = node.leftChild;
        } else {
            break;
        }
    }
    return node;
}

/**
 * Get a very simplified version of a plan, which only includes nodeType and nesting structure.
 */
function getPlanSkeleton(node) {
    const keepKeys = [
        'nodeType',

        'queryPlanner',
        'winningPlan',
        'optimizerPlan',
        'child',
        'children',
        'leftChild',
        'rightChild',
    ];

    if (Array.isArray(node)) {
        return node.map(n => getPlanSkeleton(n));
    } else if (node === null || typeof node !== 'object') {
        return node;
    } else {
        return Object.fromEntries(Object.keys(node)
                                      .filter(key => keepKeys.includes(key))
                                      .map(key => [key, getPlanSkeleton(node[key])]));
    }
}

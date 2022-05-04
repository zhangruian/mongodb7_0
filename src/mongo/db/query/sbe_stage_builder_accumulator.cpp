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

#include "mongo/platform/basic.h"

#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
namespace {

std::unique_ptr<sbe::EExpression> wrapMinMaxArg(StageBuilderState& state,
                                                std::unique_ptr<sbe::EExpression> arg) {
    return makeLocalBind(state.frameIdGenerator,
                         [](sbe::EVariable input) {
                             return sbe::makeE<sbe::EIf>(
                                 generateNullOrMissing(input),
                                 makeConstant(sbe::value::TypeTags::Nothing, 0),
                                 input.clone());
                         },
                         std::move(arg));
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorMin(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    auto collatorSlot = state.data->env->getSlotIfExists("collator"_sd);
    if (collatorSlot) {
        aggs.push_back(makeFunction("collMin"_sd,
                                    sbe::makeE<sbe::EVariable>(*collatorSlot),
                                    wrapMinMaxArg(state, std::move(arg))));
    } else {
        aggs.push_back(makeFunction("min"_sd, wrapMinMaxArg(state, std::move(arg))));
    }
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeMin(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& minSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    // We can get away with not building a project stage since there's no finalize step but we
    // will stick the slot into an EVariable in case a $min is one of many group clauses and it
    // can be combined into a final project stage.
    tassert(5754702,
            str::stream() << "Expected one input slot for finalization of min, got: "
                          << minSlots.size(),
            minSlots.size() == 1);
    return {makeFillEmptyNull(makeVariable(minSlots[0])), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorMax(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    auto collatorSlot = state.data->env->getSlotIfExists("collator"_sd);
    if (collatorSlot) {
        aggs.push_back(makeFunction("collMax"_sd,
                                    sbe::makeE<sbe::EVariable>(*collatorSlot),
                                    wrapMinMaxArg(state, std::move(arg))));
    } else {
        aggs.push_back(makeFunction("max"_sd, wrapMinMaxArg(state, std::move(arg))));
    }
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeMax(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& maxSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return {makeFillEmptyNull(makeVariable(maxSlots[0])), std::move(inputStage)};
}


std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorFirst(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("first", makeFillEmptyNull(std::move(arg))));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorLast(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("last", makeFillEmptyNull(std::move(arg))));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorAvg(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // 'aggDoubleDoubleSum' will ignore non-numeric values automatically.
    aggs.push_back(makeFunction("aggDoubleDoubleSum", arg->clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = makeLocalBind(state.frameIdGenerator,
                                [](sbe::EVariable input) {
                                    return sbe::makeE<sbe::EIf>(
                                        makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                     generateNullOrMissing(input),
                                                     generateNonNumericCheck(input)),
                                        makeConstant(sbe::value::TypeTags::NumberInt64, 0),
                                        makeConstant(sbe::value::TypeTags::NumberInt64, 1));
                                },
                                std::move(arg));
    auto counterExpr = makeFunction("sum", std::move(addend));
    aggs.push_back(std::move(counterExpr));

    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeAvg(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& aggSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(5754703,
            str::stream() << "Expected two slots to finalize avg, got: " << aggSlots.size(),
            aggSlots.size() == 2);

    if (state.needsMerge) {
        // To support the sharding behavior, the mongos splits $group into two separate $group
        // stages one at the mongos-side and the other at the shard-side. This stage builder builds
        // the shard-side plan. The shard-side $avg accumulator is responsible to return the partial
        // avg in the form of {count: val, ps: array_val}.
        auto sumResult = makeVariable(aggSlots[0]);
        auto countResult = makeVariable(aggSlots[1]);
        auto partialSumExpr = makeFunction("doubleDoublePartialSumFinalize", sumResult->clone());

        // Returns {count: val, ps: array_val}.
        auto partialAvgFinalize =
            makeNewObjFunction(FieldPair{countName, countResult->clone()},
                               FieldPair{partialSumName, partialSumExpr->clone()});

        return {std::move(partialAvgFinalize), std::move(inputStage)};
    } else {
        // If we've encountered any numeric input, the counter would contain a positive integer.
        // Unlike $sum, when there is no numeric input, $avg should return null.
        auto finalizingExpression = sbe::makeE<sbe::EIf>(
            makeBinaryOp(sbe::EPrimBinary::eq,
                         makeVariable(aggSlots[1]),
                         makeConstant(sbe::value::TypeTags::NumberInt64, 0)),
            makeConstant(sbe::value::TypeTags::Null, 0),
            makeBinaryOp(sbe::EPrimBinary::div,
                         makeFunction("doubleDoubleSumFinalize", makeVariable(aggSlots[0])),
                         makeVariable(aggSlots[1])));

        return {std::move(finalizingExpression), std::move(inputStage)};
    }
}

namespace {
std::tuple<bool, boost::optional<sbe::value::TypeTags>, boost::optional<sbe::value::Value>>
getCountAddend(const AccumulationExpression& expr) {
    auto constArg = dynamic_cast<ExpressionConstant*>(expr.argument.get());
    if (!constArg) {
        return {false, boost::none, boost::none};
    }

    auto value = constArg->getValue();
    switch (value.getType()) {
        case BSONType::NumberInt:
            return {true,
                    sbe::value::TypeTags::NumberInt32,
                    sbe::value::bitcastFrom<int>(value.getInt())};
        case BSONType::NumberLong:
            return {true,
                    sbe::value::TypeTags::NumberInt64,
                    sbe::value::bitcastFrom<long long>(value.getLong())};
        case BSONType::NumberDouble:
            return {true,
                    sbe::value::TypeTags::NumberDouble,
                    sbe::value::bitcastFrom<double>(value.getDouble())};
        default:
            // 'value' is NumberDecimal type in which case, 'sum' function may not be efficient
            // due to decimal data copying which involves memory allocation. To avoid such
            // inefficiency, does not support NumberDecimal type for this optimization.
            return {false, boost::none, boost::none};
    }
}
}  // namespace

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorSum(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // Optimize for a count-like accumulator like {$sum: 1}.
    if (auto [isCount, addendTag, addendVal] = getCountAddend(expr); isCount) {
        aggs.push_back(makeFunction("sum", makeConstant(*addendTag, *addendVal)));
        return {std::move(aggs), std::move(inputStage)};
    }

    aggs.push_back(makeFunction("aggDoubleDoubleSum", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeSum(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& sumSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755300,
            str::stream() << "Expected one input slot for finalization of sum, got: "
                          << sumSlots.size(),
            sumSlots.size() == 1);

    if (state.needsMerge) {
        // Serialize the full state of the partial sum result to avoid incorrect results for certain
        // data set which are composed of 'NumberDecimal' values which cancel each other when being
        // summed and other numeric type values which contribute mostly to sum result and a partial
        // sum of some of 'NumberDecimal' values and other numeric type values happen to lose
        // precision because 'NumberDecimal' can't represent the partial sum precisely, or the other
        // way around.
        //
        // For example, [{n: 1e+34}, {n: NumberDecimal("0,1")}, {n: NumberDecimal("0.11")}, {n:
        // -1e+34}].
        //
        // More fundamentally, addition is neither commutative nor associative on computer. So, it's
        // desirable to keep the full state of the partial sum along the way to maintain the result
        // as close to the real truth as possible until all additions are done.
        return {makeFunction("doubleDoublePartialSumFinalize", makeVariable(sumSlots[0])),
                std::move(inputStage)};
    }

    if (auto [isCount, tag, val] = getCountAddend(expr); isCount) {
        // The accumulation result is a scalar value. So, the final project is not necessary.
        return {nullptr, std::move(inputStage)};
    }

    return {makeFunction("doubleDoubleSumFinalize", makeVariable(sumSlots[0])),
            std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorAddToSet(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    const int cap = internalQueryMaxAddToSetBytes.load();
    auto collatorSlot = state.data->env->getSlotIfExists("collator"_sd);
    if (collatorSlot) {
        aggs.push_back(makeFunction(
            "collAddToSetCapped"_sd,
            sbe::makeE<sbe::EVariable>(*collatorSlot),
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    } else {
        aggs.push_back(makeFunction(
            "addToSetCapped",
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    }
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeCappedAccumulator(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& accSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(6526500,
            str::stream() << "Expected one input slot for finalization of capped accumulator, got: "
                          << accSlots.size(),
            accSlots.size() == 1);

    // 'accSlots[0]' should contain an array of size two, where the front element is the accumulated
    // values and the back element is their cumulative size in bytes. We just ignore the size
    // because if it exceeded the size cap, we should have thrown an error during accumulation.
    auto pushFinalize =
        makeFunction("getElement",
                     makeVariable(accSlots[0]),
                     makeConstant(sbe::value::TypeTags::NumberInt32,
                                  static_cast<int>(sbe::vm::AggArrayWithSize::kValues)));

    return {std::move(pushFinalize), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorPush(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    const int cap = internalQueryMaxPushBytes.load();
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(
        "addToArrayCapped"_sd,
        std::move(arg),
        makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorStdDev(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggStdDev", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::unique_ptr<sbe::EExpression> buildFinalizePartialStdDev(sbe::value::SlotId stdDevSlot) {
    // To support the sharding behavior, the mongos splits $group into two separate $group
    // stages one at the mongos-side and the other at the shard-side. This stage builder builds
    // the shard-side plan. The shard-side $stdDevSamp and $stdDevPop accumulators are responsible
    // to return the partial stddev in the form of {m2: val1, mean: val2, count: val3}.
    auto stdDevResult = makeVariable(stdDevSlot);

    return makeNewObjFunction(
        FieldPair{
            "m2"_sd,
            makeFunction("getElement",
                         stdDevResult->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningM2)))},
        FieldPair{"mean"_sd,
                  makeFunction(
                      "getElement",
                      stdDevResult->clone(),
                      makeConstant(sbe::value::TypeTags::NumberInt32,
                                   static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningMean)))},
        FieldPair{
            "count"_sd,
            makeFunction("getElement",
                         stdDevResult->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(sbe::vm::AggStdDevValueElems::kCount)))});
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeStdDevPop(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& stdDevSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755204,
            str::stream() << "Expected one input slot for finalization of stdDevPop, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return {buildFinalizePartialStdDev(stdDevSlots[0]), std::move(inputStage)};
    } else {
        auto stdDevPopFinalize = makeFunction("stdDevPopFinalize", makeVariable(stdDevSlots[0]));
        return {std::move(stdDevPopFinalize), std::move(inputStage)};
    }
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeStdDevSamp(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& stdDevSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755209,
            str::stream() << "Expected one input slot for finalization of stdDevSamp, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return {buildFinalizePartialStdDev(stdDevSlots[0]), std::move(inputStage)};
    } else {
        auto stdDevSampFinalize = makeFunction("stdDevSampFinalize", makeVariable(stdDevSlots[0]));
        return {std::move(stdDevSampFinalize), std::move(inputStage)};
    }
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorMergeObjects(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    auto filterExpr = makeLocalBind(
        state.frameIdGenerator,
        [](sbe::EVariable input) {
            return makeBinaryOp(
                sbe::EPrimBinary::logicOr,
                generateNullOrMissing(input),
                makeBinaryOp(sbe::EPrimBinary::logicOr,
                             makeFunction("isObject", input.clone()),
                             sbe::makeE<sbe::EFail>(ErrorCodes::Error{5911200},
                                                    "$mergeObjects only supports objects")));
        },
        arg->clone());

    inputStage = makeFilter<false>(std::move(inputStage), std::move(filterExpr), planNodeId);

    aggs.push_back(makeFunction("mergeObjects", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}
};  // namespace

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildArgument(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage stage,
    boost::optional<sbe::value::SlotId> optionalRootSlot,
    PlanNodeId planNodeId) {
    auto [argExpr, outStage] = generateExpression(
        state, acc.expr.argument.get(), std::move(stage), optionalRootSlot, planNodeId);
    return {argExpr.extractExpr(), std::move(outStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage inputStage,
    std::unique_ptr<sbe::EExpression> inputExpr,
    PlanNodeId planNodeId) {
    using BuildAccumulatorFn =
        std::function<std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage>(
            StageBuilderState&,
            const AccumulationExpression&,
            std::unique_ptr<sbe::EExpression>,
            EvalStage,
            PlanNodeId)>;

    static const StringDataMap<BuildAccumulatorFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildAccumulatorMin},
        {AccumulatorMax::kName, &buildAccumulatorMax},
        {AccumulatorFirst::kName, &buildAccumulatorFirst},
        {AccumulatorLast::kName, &buildAccumulatorLast},
        {AccumulatorAvg::kName, &buildAccumulatorAvg},
        {AccumulatorAddToSet::kName, &buildAccumulatorAddToSet},
        {AccumulatorSum::kName, &buildAccumulatorSum},
        {AccumulatorPush::kName, &buildAccumulatorPush},
        {AccumulatorMergeObjects::kName, &buildAccumulatorMergeObjects},
        {AccumulatorStdDevPop::kName, &buildAccumulatorStdDev},
        {AccumulatorStdDevSamp::kName, &buildAccumulatorStdDev},
    };

    auto accExprName = acc.expr.name;
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       state,
                       acc.expr,
                       std::move(inputExpr),
                       std::move(inputStage),
                       planNodeId);
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalize(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& aggSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    using BuildFinalizeFn = std::function<std::pair<std::unique_ptr<sbe::EExpression>, EvalStage>(
        StageBuilderState&,
        const AccumulationExpression&,
        sbe::value::SlotVector,
        EvalStage,
        PlanNodeId)>;

    static const StringDataMap<BuildFinalizeFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildFinalizeMin},
        {AccumulatorMax::kName, &buildFinalizeMax},
        {AccumulatorFirst::kName, nullptr},
        {AccumulatorLast::kName, nullptr},
        {AccumulatorAvg::kName, &buildFinalizeAvg},
        {AccumulatorAddToSet::kName, &buildFinalizeCappedAccumulator},
        {AccumulatorSum::kName, &buildFinalizeSum},
        {AccumulatorPush::kName, &buildFinalizeCappedAccumulator},
        {AccumulatorMergeObjects::kName, nullptr},
        {AccumulatorStdDevPop::kName, &buildFinalizeStdDevPop},
        {AccumulatorStdDevSamp::kName, &buildFinalizeStdDevSamp},
    };

    auto accExprName = acc.expr.name;
    uassert(5754700,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    if (auto fn = kAccumulatorBuilders.at(accExprName); fn) {
        return std::invoke(fn, state, acc.expr, aggSlots, std::move(inputStage), planNodeId);
    } else {
        // nullptr for 'EExpression' signifies that no final project is necessary.
        return {nullptr, std::move(inputStage)};
    }
}
}  // namespace mongo::stage_builder

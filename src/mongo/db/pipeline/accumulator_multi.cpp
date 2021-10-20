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

#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/util/version/releases.h"

namespace mongo {
using FirstLastSense = AccumulatorFirstLastN::Sense;
using MinMaxSense = AccumulatorMinMax::Sense;

// TODO SERVER-52247 Replace boost::none with 'gFeatureFlagExactTopNAccumulator.getVersion()' below
// once 'gFeatureFlagExactTopNAccumulator' is set to true by default and is configured with an FCV.
REGISTER_ACCUMULATOR_CONDITIONALLY(
    maxN,
    AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMax>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    minN,
    AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMin>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_EXPRESSION_CONDITIONALLY(
    maxN,
    AccumulatorMinMaxN::parseExpression<MinMaxSense::kMax>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_EXPRESSION_CONDITIONALLY(
    minN,
    AccumulatorMinMaxN::parseExpression<MinMaxSense::kMin>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    firstN,
    AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kFirst>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    lastN,
    AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kLast>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_EXPRESSION_CONDITIONALLY(
    firstN,
    AccumulatorFirstLastN::parseExpression<FirstLastSense::kFirst>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_EXPRESSION_CONDITIONALLY(
    lastN,
    AccumulatorFirstLastN::parseExpression<FirstLastSense::kLast>,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
// TODO SERVER-57884 Add $firstN/$lastN as window functions.
// TODO SERVER-57886 Add $topN/$bottomN/$top/$bottom as window functions.
REGISTER_ACCUMULATOR_CONDITIONALLY(
    topN,
    (AccumulatorTopBottomN<TopBottomSense::kTop, false>::parseTopBottomN),
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    bottomN,
    (AccumulatorTopBottomN<TopBottomSense::kBottom, false>::parseTopBottomN),
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    top,
    (AccumulatorTopBottomN<TopBottomSense::kTop, true>::parseTopBottomN),
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
REGISTER_ACCUMULATOR_CONDITIONALLY(
    bottom,
    (AccumulatorTopBottomN<TopBottomSense::kBottom, true>::parseTopBottomN),
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::none,
    feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());

AccumulatorN::AccumulatorN(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _maxMemUsageBytes(internalQueryMaxNAccumulatorBytes.load()) {}

long long AccumulatorN::validateN(const Value& input) {
    // Obtain the value for 'n' and error if it's not a positive integral.
    uassert(5787902,
            str::stream() << "Value for 'n' must be of integral type, but found "
                          << input.toString(),
            input.numeric());
    auto n = input.coerceToLong();
    uassert(5787903,
            str::stream() << "Value for 'n' must be of integral type, but found "
                          << input.toString(),
            n == input.coerceToDouble());
    uassert(5787908, str::stream() << "'n' must be greater than 0, found " << n, n > 0);
    return n;
}
void AccumulatorN::startNewGroup(const Value& input) {
    _n = validateN(input);
}

void AccumulatorN::processInternal(const Value& input, bool merging) {
    tassert(5787802, "'n' must be initialized", _n);

    if (merging) {
        tassert(5787803, "input must be an array when 'merging' is true", input.isArray());
        auto array = input.getArray();
        for (auto&& val : array) {
            processValue(val);
        }
    } else {
        processValue(input);
    }
}

AccumulatorMinMaxN::AccumulatorMinMaxN(ExpressionContext* const expCtx, MinMaxSense sense)
    : AccumulatorN(expCtx),
      _set(expCtx->getValueComparator().makeOrderedValueMultiset()),
      _sense(sense) {
    _memUsageBytes = sizeof(*this);
}

const char* AccumulatorMinMaxN::getOpName() const {
    if (_sense == MinMaxSense::kMin) {
        return AccumulatorMinN::getName();
    } else {
        return AccumulatorMaxN::getName();
    }
}

Document AccumulatorMinMaxN::serialize(boost::intrusive_ptr<Expression> initializer,
                                       boost::intrusive_ptr<Expression> argument,
                                       bool explain) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, explain, args);
    return DOC(getOpName() << args.freeze());
}

template <MinMaxSense s>
boost::intrusive_ptr<Expression> AccumulatorMinMaxN::parseExpression(
    ExpressionContext* const expCtx, BSONElement exprElement, const VariablesParseState& vps) {
    auto accExpr = AccumulatorMinMaxN::parseMinMaxN<s>(expCtx, exprElement, vps);
    if constexpr (s == MinMaxSense::kMin) {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorMinN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    } else {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorMaxN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    }
}

std::tuple<boost::intrusive_ptr<Expression>, boost::intrusive_ptr<Expression>>
AccumulatorN::parseArgs(ExpressionContext* const expCtx,
                        const BSONObj& args,
                        VariablesParseState vps) {
    boost::intrusive_ptr<Expression> n;
    boost::intrusive_ptr<Expression> output;
    for (auto&& element : args) {
        auto fieldName = element.fieldNameStringData();
        if (fieldName == kFieldNameOutput) {
            output = Expression::parseOperand(expCtx, element, vps);
        } else if (fieldName == kFieldNameN) {
            n = Expression::parseOperand(expCtx, element, vps);
        } else {
            uasserted(5787901, str::stream() << "Unknown argument for 'n' operator: " << fieldName);
        }
    }
    uassert(5787906, str::stream() << "Missing value for '" << kFieldNameN << "'", n);
    uassert(5787907, str::stream() << "Missing value for '" << kFieldNameOutput << "'", output);
    return std::make_tuple(n, output);
}

void AccumulatorN::serializeHelper(const boost::intrusive_ptr<Expression>& initializer,
                                   const boost::intrusive_ptr<Expression>& argument,
                                   bool explain,
                                   MutableDocument& md) {
    md.addField(kFieldNameN, Value(initializer->serialize(explain)));
    md.addField(kFieldNameOutput, Value(argument->serialize(explain)));
}

template <MinMaxSense s>
AccumulationExpression AccumulatorMinMaxN::parseMinMaxN(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;
    auto name = [] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::getName();
        } else {
            return AccumulatorMaxN::getName();
        }
    }();

    uassert(5787900,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    auto [n, output] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::create(expCtx);
        } else {
            return AccumulatorMaxN::create(expCtx);
        }
    };

    return {std::move(n), std::move(output), std::move(factory), name};
}

void AccumulatorMinMaxN::processValue(const Value& val) {
    // Ignore nullish values.
    if (val.nullish())
        return;

    // Only compare if we have 'n' elements.
    if (static_cast<long long>(_set.size()) == *_n) {
        // Get an iterator to the element we want to compare against.
        auto cmpElem = _sense == MinMaxSense::kMin ? std::prev(_set.end()) : _set.begin();

        auto cmp = getExpressionContext()->getValueComparator().compare(*cmpElem, val) * _sense;
        if (cmp > 0) {
            _memUsageBytes -= cmpElem->getApproximateSize();
            _set.erase(cmpElem);
        } else {
            return;
        }
    }
    _memUsageBytes += val.getApproximateSize();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and cannot spill to disk. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
    _set.emplace(val);
}

Value AccumulatorMinMaxN::getValue(bool toBeMerged) {
    // Return the values in ascending order for 'kMin' and descending order for 'kMax'.
    return Value(_sense == MinMaxSense::kMin ? std::vector<Value>(_set.begin(), _set.end())
                                             : std::vector<Value>(_set.rbegin(), _set.rend()));
}

void AccumulatorMinMaxN::reset() {
    _set = getExpressionContext()->getValueComparator().makeOrderedValueMultiset();
    _memUsageBytes = sizeof(*this);
}

const char* AccumulatorMinN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorMinN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorMinN>(expCtx);
}

const char* AccumulatorMaxN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorMaxN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorMaxN>(expCtx);
}

AccumulatorFirstLastN::AccumulatorFirstLastN(ExpressionContext* const expCtx, FirstLastSense sense)
    : AccumulatorN(expCtx), _deque(std::deque<Value>()), _variant(sense) {
    _memUsageBytes = sizeof(*this);
}

// TODO SERVER-59327 Deduplicate with the block in 'AccumulatorMinMaxN::parseMinMaxN'
template <FirstLastSense v>
AccumulationExpression AccumulatorFirstLastN::parseFirstLastN(ExpressionContext* const expCtx,
                                                              BSONElement elem,
                                                              VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;
    auto name = [] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::getName();
        } else {
            return AccumulatorLastN::getName();
        }
    }();

    uassert(5787801,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    auto obj = elem.embeddedObject();

    auto [n, output] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::create(expCtx);
        } else {
            return AccumulatorLastN::create(expCtx);
        }
    };

    return {std::move(n), std::move(output), std::move(factory), name};
}

void AccumulatorFirstLastN::processValue(const Value& val) {
    // Only insert in the lastN case if we have 'n' elements.
    if (static_cast<long long>(_deque.size()) == *_n) {
        if (_variant == Sense::kLast) {
            _memUsageBytes -= _deque.front().getApproximateSize();
            _deque.pop_front();
        } else {
            return;
        }
    }

    _memUsageBytes += val.getApproximateSize();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and cannot spill to disk. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
    _deque.push_back(val);
}

const char* AccumulatorFirstLastN::getOpName() const {
    if (_variant == Sense::kFirst) {
        return AccumulatorFirstN::getName();
    } else {
        return AccumulatorLastN::getName();
    }
}

Document AccumulatorFirstLastN::serialize(boost::intrusive_ptr<Expression> initializer,
                                          boost::intrusive_ptr<Expression> argument,
                                          bool explain) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, explain, args);
    return DOC(getOpName() << args.freeze());
}

template <FirstLastSense s>
boost::intrusive_ptr<Expression> AccumulatorFirstLastN::parseExpression(
    ExpressionContext* expCtx, BSONElement exprElement, const VariablesParseState& vps) {
    auto accExpr = AccumulatorFirstLastN::parseFirstLastN<s>(expCtx, exprElement, vps);
    if constexpr (s == FirstLastSense::kFirst) {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorFirstN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    } else {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorLastN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    }
}

void AccumulatorFirstLastN::reset() {
    _deque = std::deque<Value>();
    _memUsageBytes = sizeof(*this);
}

Value AccumulatorFirstLastN::getValue(bool toBeMerged) {
    return Value(std::vector<Value>(_deque.begin(), _deque.end()));
}

const char* AccumulatorFirstN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorFirstN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorFirstN>(expCtx);
}

const char* AccumulatorLastN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorLastN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorLastN>(expCtx);
}

// TODO SERVER-59327 Refactor other operators to use this parse function.
template <bool single>
std::tuple<boost::intrusive_ptr<Expression>, BSONElement, boost::optional<BSONObj>>
accumulatorNParseArgs(ExpressionContext* expCtx,
                      const BSONElement& elem,
                      const char* name,
                      bool needSortBy,
                      const VariablesParseState& vps) {
    uassert(5788001,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    // Extract fields from specification object. sortBy and output are not immediately parsed into
    // Expressions so that they can easily still be manipulated and processed in the special case of
    // AccumulatorTopBottomN.
    boost::optional<BSONObj> sortBy;
    boost::optional<BSONElement> output;
    boost::intrusive_ptr<Expression> n;
    for (auto&& element : obj) {
        auto fieldName = element.fieldNameStringData();
        if constexpr (!single) {
            if (fieldName == AccumulatorN::kFieldNameN) {
                n = Expression::parseOperand(expCtx, element, vps);
                continue;
            }
        }
        if (fieldName == AccumulatorN::kFieldNameOutput) {
            output = element;
        } else if (fieldName == AccumulatorN::kFieldNameSortBy && needSortBy) {
            sortBy = element.Obj();
        } else {
            uasserted(5788002, str::stream() << "Unknown argument to " << name << " " << fieldName);
        }
    }

    // Make sure needed arguments were found.
    if constexpr (single) {
        n = ExpressionConstant::create(expCtx, Value(1));
    } else {
        uassert(
            5788003, str::stream() << "Missing value for '" << AccumulatorN::kFieldNameN << "'", n);
    }
    uassert(5788004,
            str::stream() << "Missing value for '" << AccumulatorN::kFieldNameOutput << "'",
            output);
    if (needSortBy) {
        uassert(5788005,
                str::stream() << "Missing value for '" << AccumulatorN::kFieldNameSortBy << "'",
                sortBy);
    }

    return {n, *output, sortBy};
}

template <TopBottomSense sense, bool single>
AccumulatorTopBottomN<sense, single>::AccumulatorTopBottomN(ExpressionContext* const expCtx,
                                                            SortPattern sp)
    : AccumulatorN(expCtx), _sortPattern(sp) {

    // Modify sortPattern to sort based on fields where they are in the evaluated argument instead
    // of where they would be in the raw document received by $group and friends.
    std::vector<SortPattern::SortPatternPart> parts;
    int sortOrder = 0;
    for (auto part : _sortPattern) {
        const auto newFieldName =
            (StringBuilder() << AccumulatorN::kFieldNameSortFields << "." << sortOrder).str();
        part.fieldPath.reset(FieldPath(newFieldName));

        // TODO SERVER-60781 will change AccumulatorTopBottomN so it has different behavior
        // Invert sort spec if $topN/top.
        if constexpr (sense == TopBottomSense::kTop) {
            // $topN usually flips sort pattern by making ascending false. for the case of textScore
            // based sorting, there is no way to sort by least relevent in a normal mongodb sort
            // specification so topN still returns the same order as bottomN (most relevent first).
            if (!part.expression) {
                part.isAscending = !part.isAscending;
            }
        }
        if (part.expression) {
            // $meta based sorting is handled earlier in the sortFields expression. See comment in
            // parseAccumulatorTopBottomNSortBy().
            part.expression = nullptr;
        }
        parts.push_back(part);
        sortOrder++;
    }
    SortPattern internalSortPattern(parts);

    _sortKeyComparator.emplace(internalSortPattern);
    _sortKeyGenerator.emplace(std::move(internalSortPattern), expCtx->getCollator());

    _memUsageBytes = sizeof(*this);

    // STL expects a less-than function not a 3-way compare function so this lambda wraps
    // SortKeyComparator.
    _map.emplace([&, this](const Value& lhs, const Value& rhs) {
        return (*this->_sortKeyComparator)(lhs, rhs) < 0;
    });
}

template <TopBottomSense sense, bool single>
const char* AccumulatorTopBottomN<sense, single>::getOpName() const {
    return AccumulatorTopBottomN<sense, single>::getName().rawData();
}

template <TopBottomSense sense, bool single>
Document AccumulatorTopBottomN<sense, single>::serialize(
    boost::intrusive_ptr<Expression> initializer,
    boost::intrusive_ptr<Expression> argument,
    bool explain) const {
    MutableDocument args;
    if constexpr (!single) {
        args.addField(kFieldNameN, Value(initializer->serialize(explain)));
    }
    auto output = argument->serialize(explain)[kFieldNameOutput];
    tassert(5788000,
            str::stream() << "expected argument expression to have " << kFieldNameOutput
                          << " field",
            !output.missing());
    args.addField(kFieldNameOutput, Value(output));
    args.addField(kFieldNameSortBy,
                  Value(_sortPattern.serialize(
                      SortPattern::SortKeySerialization::kForPipelineSerialization)));
    return DOC(getOpName() << args.freeze());
}

template <TopBottomSense sense>
std::pair<SortPattern, BSONArray> parseAccumulatorTopBottomNSortBy(ExpressionContext* const expCtx,
                                                                   BSONObj sortBy) {
    SortPattern sortPattern(sortBy, expCtx);
    BSONArrayBuilder sortFieldsExpBab;
    BSONObjIterator sortByBoi(sortBy);
    int sortOrder = 0;
    for (const auto& part : sortPattern) {
        const auto fieldName = sortByBoi.next().fieldNameStringData();
        const auto newFieldName =
            (StringBuilder() << AccumulatorN::kFieldNameSortFields << "." << sortOrder).str();

        if (part.expression) {
            // In a scenario where we are sorting by metadata (for example if sortBy is
            // {text: {$meta: "textScore"}}) we cant use ["$text"] as the sortFields expression
            // since the evaluated argument wouldn't have the same metadata as the original
            // document. Instead we use [{$meta: "textScore"}] as the sortFields expression so the
            // sortFields array contains the data we need for sorting.
            const auto serialized = part.expression->serialize(false);
            sortFieldsExpBab.append(serialized.getDocument().toBson());
        } else {
            sortFieldsExpBab.append((StringBuilder() << "$" << fieldName).str());
        }
        sortOrder++;
    }
    return {sortPattern, sortFieldsExpBab.arr()};
}

template <TopBottomSense sense, bool single>
AccumulationExpression AccumulatorTopBottomN<sense, single>::parseTopBottomN(
    ExpressionContext* const expCtx, BSONElement elem, VariablesParseState vps) {
    auto name = AccumulatorTopBottomN<sense, single>::getName();

    const auto [n, output, sortBy] =
        accumulatorNParseArgs<single>(expCtx, elem, name.rawData(), true, vps);

    auto [sortPattern, sortFieldsExp] = parseAccumulatorTopBottomNSortBy<sense>(expCtx, *sortBy);

    // Construct argument expression. If given sortBy: {field1: 1, field2: 1} it will be shaped like
    // {output: <output expression>, sortFields: ["$field1", "$field2"]}. This projects out only the
    // fields we need for sorting so we can use SortKeyComparator without copying the entire
    // document. This argument expression will be evaluated and become the input to processValue.
    boost::intrusive_ptr<Expression> argument = Expression::parseObject(
        expCtx, BSON(output << AccumulatorN::kFieldNameSortFields << sortFieldsExp), vps);

    auto factory = [expCtx, sortPattern = std::move(sortPattern)] {
        return make_intrusive<AccumulatorTopBottomN<sense, single>>(expCtx, sortPattern);
    };

    return {std::move(n), std::move(argument), std::move(factory), name};
}

template <TopBottomSense sense, bool single>
boost::intrusive_ptr<AccumulatorState> AccumulatorTopBottomN<sense, single>::create(
    ExpressionContext* expCtx, BSONObj sortBy) {
    return make_intrusive<AccumulatorTopBottomN<sense, single>>(
        expCtx, parseAccumulatorTopBottomNSortBy<sense>(expCtx, sortBy).first);
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::processValue(const Value& val) {
    tassert(5788014,
            str::stream() << "processValue of " << getName() << "should have recieved an object",
            val.isObject());

    Value output = val[AccumulatorN::kFieldNameOutput];
    Value sortKey;

    // In the case that processValue() is getting called in the context of merging, a previous
    // processValue has already generated the sortKey for us, so we don't need to regenerate it.
    Value generatedSortKey = val[kFieldNameGeneratedSortKey];
    if (!generatedSortKey.missing()) {
        sortKey = generatedSortKey;
    } else {
        sortKey = _sortKeyGenerator->computeSortKeyFromDocument(val.getDocument());
    }
    KeyOutPair keyOutPair(sortKey, output);

    // Only compare if we have 'n' elements.
    if (static_cast<long long>(_map->size()) == *_n) {
        // Get an iterator to the element we want to compare against.
        auto cmpElem = std::prev(_map->end());

        // TODO SERVER-60781 will change AccumulatorTopBottomN so it has different behavior. $topN
        // will insert items greater than the min and $bottomN will insert items less than the max.
        auto cmp = (*_sortKeyComparator)(cmpElem->first, keyOutPair.first);
        // When the sort key produces a tie we keep the first value seen.
        if (cmp > 0) {
            _memUsageBytes -= cmpElem->first.getApproximateSize() +
                cmpElem->second.getApproximateSize() + sizeof(KeyOutPair);
            _map->erase(cmpElem);
        } else {
            return;
        }
    }
    _memUsageBytes +=
        sortKey.getApproximateSize() + output.getApproximateSize() + sizeof(KeyOutPair);
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and cannot spill to disk. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
    _map->emplace(keyOutPair);
}

template <TopBottomSense sense, bool single>
Value AccumulatorTopBottomN<sense, single>::getValue(bool toBeMerged) {
    std::vector<Value> result;
    for (const auto& keyOutPair : *_map) {
        if (toBeMerged) {
            result.emplace_back(BSON(kFieldNameGeneratedSortKey
                                     << keyOutPair.first << kFieldNameOutput << keyOutPair.second));
        } else {
            result.push_back(keyOutPair.second);
        }
    };

    if constexpr (!single) {
        return Value(result);
    } else {
        tassert(5788015,
                str::stream() << getName() << " group did not contain exactly one value",
                result.size() == 1);
        if (toBeMerged) {
            return Value(result);
        } else {
            return Value(result[0]);
        }
    }
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::reset() {
    _map->clear();
    _memUsageBytes = sizeof(*this);
}

// Explicitly specify the following classes should generated and should live in this compilation
// unit.
template class AccumulatorTopBottomN<TopBottomSense::kBottom, false>;
template class AccumulatorTopBottomN<TopBottomSense::kBottom, true>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, false>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, true>;

}  // namespace mongo

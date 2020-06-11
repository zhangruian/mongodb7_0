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

#include "mongo/db/exec/sbe/stages/hash_agg.h"

#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
HashAggStage::HashAggStage(std::unique_ptr<PlanStage> input,
                           value::SlotVector gbs,
                           value::SlotMap<std::unique_ptr<EExpression>> aggs)
    : PlanStage("group"_sd), _gbs(std::move(gbs)), _aggs(std::move(aggs)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> HashAggStage::clone() const {
    value::SlotMap<std::unique_ptr<EExpression>> aggs;
    for (auto& [k, v] : _aggs) {
        aggs.emplace(k, v->clone());
    }
    return std::make_unique<HashAggStage>(_children[0]->clone(), _gbs, std::move(aggs));
}

void HashAggStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    value::SlotSet dupCheck;
    size_t counter = 0;
    // Process group by columns.
    for (auto& slot : _gbs) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(4822827, str::stream() << "duplicate field: " << slot, inserted);

        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        _outKeyAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, counter++));
        _outAccessors[slot] = _outKeyAccessors.back().get();
    }

    counter = 0;
    for (auto& [slot, expr] : _aggs) {
        auto [it, inserted] = dupCheck.emplace(slot);
        // Some compilers do not allow to capture local bindings by lambda functions (the one
        // is used implicitly in uassert below), so we need a local variable to construct an
        // error message.
        const auto slotId = slot;
        uassert(4822828, str::stream() << "duplicate field: " << slotId, inserted);

        _outAggAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, counter++));
        _outAccessors[slot] = _outAggAccessors.back().get();

        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = _outAggAccessors.back().get();

        _aggCodes.emplace_back(expr->compile(ctx));
        ctx.aggExpression = false;
    }
    _compiled = true;
}

value::SlotAccessor* HashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second;
        }
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }

    return ctx.getAccessor(slot);
}

void HashAggStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);

    value::MaterializedRow key;
    while (_children[0]->getNext() == PlanState::ADVANCED) {
        key._fields.resize(_inKeyAccessors.size());
        // Copy keys in order to do the lookup.
        size_t idx = 0;
        for (auto& p : _inKeyAccessors) {
            auto [tag, val] = p->getViewOfValue();
            key._fields[idx++].reset(false, tag, val);
        }

        auto [it, inserted] = _ht.emplace(std::move(key), value::MaterializedRow{});
        if (inserted) {
            // Copy keys.
            const_cast<value::MaterializedRow&>(it->first).makeOwned();
            // Initialize accumulators.
            it->second._fields.resize(_outAggAccessors.size());
        }

        // Accumulate.
        _htIt = it;
        for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
            auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
            _outAggAccessors[idx]->reset(owned, tag, val);
        }
    }

    _children[0]->close();

    _htIt = _ht.end();
}

PlanState HashAggStage::getNext() {
    if (_htIt == _ht.end()) {
        _htIt = _ht.begin();
    } else {
        ++_htIt;
    }

    if (_htIt == _ht.end()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> HashAggStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* HashAggStage::getSpecificStats() const {
    return nullptr;
}

void HashAggStage::close() {
    _commonStats.closes++;
}

std::vector<DebugPrinter::Block> HashAggStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "group");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _gbs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _gbs[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    bool first = true;
    for (auto& p : _aggs) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, p.first);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, p.second->debugPrint());
        first = false;
    }
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}
}  // namespace sbe
}  // namespace mongo

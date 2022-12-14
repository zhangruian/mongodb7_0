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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> EvalExpr::extractExpr(optimizer::SlotVarMap& varMap) {
    if (hasSlot()) {
        return sbe::makeE<sbe::EVariable>(stdx::get<sbe::value::SlotId>(_storage));
    }

    if (hasABT()) {
        return abtToExpr(stdx::get<optimizer::ABT>(_storage), varMap);
    }

    if (stdx::holds_alternative<bool>(_storage)) {
        return std::unique_ptr<sbe::EExpression>{};
    }

    return std::move(stdx::get<std::unique_ptr<sbe::EExpression>>(_storage));
}

optimizer::ABT EvalExpr::extractABT(optimizer::SlotVarMap& varMap) {
    if (hasSlot()) {
        auto slotId = stdx::get<sbe::value::SlotId>(_storage);
        auto varName = makeVariableName(slotId);
        varMap.emplace(varName, slotId);
        return optimizer::make<optimizer::Variable>(varName);
    }

    tassert(6950800,
            "Unexpected: extractABT() method invoked on an EExpression object",
            stdx::holds_alternative<optimizer::ABT>(_storage));

    return std::move(stdx::get<optimizer::ABT>(_storage));
}

}  // namespace mongo::stage_builder

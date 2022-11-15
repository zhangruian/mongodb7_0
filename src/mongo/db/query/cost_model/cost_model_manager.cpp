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

#include "mongo/db/query/cost_model/cost_model_manager.h"

#include "mongo/db/query/cost_model/cost_model_on_update.h"

namespace mongo::cost_model {
namespace {
/**
 * Populate given cost model coefficients object with default values.
 */
void initializeCoefficients(CostModelCoefficients& coefficients) {
    // These cost should reflect estimated aggregated execution time in milliseconds.
    constexpr double ms = 1.0e-3;

    // Startup cost of an operator. This is the minimal cost of an operator since it is
    // present even if it doesn't process any input.
    // TODO: calibrate the cost individually for each operator
    coefficients.setDefaultStartupCost(0.000001);

    // TODO: collection scan should depend on the width of the doc.
    // TODO: the actual measured cost is (0.4 * ms), however we increase it here because currently
    // it is not possible to estimate the cost of a collection scan vs a full index scan.
    coefficients.setScanIncrementalCost(0.6 * ms);
    coefficients.setScanStartupCost(0.000001);

    // TODO: cost(N fields) ~ (0.55 + 0.025 * N)
    coefficients.setIndexScanIncrementalCost(0.5 * ms);
    coefficients.setIndexScanStartupCost(0.000001);

    // TODO: cost(N fields) ~ 0.7 + 0.19 * N
    coefficients.setSeekCost(2.0 * ms);
    coefficients.setSeekStartupCost(0.000001);

    // TODO: take the expression into account.
    // cost(N conditions) = 0.2 + N * ???
    coefficients.setFilterIncrementalCost(0.2 * ms);
    coefficients.setFilterStartupCost(0.000001);

    // TODO: the cost of projection depends on number of fields: cost(N fields) ~ 0.1 + 0.2 * N
    coefficients.setEvalIncrementalCost(2.0 * ms);
    coefficients.setEvalStartupCost(0.000001);

    // TODO: cost(N fields) ~ 0.04 + 0.03*(N^2)
    coefficients.setGroupByIncrementalCost(0.07 * ms);
    coefficients.setGroupByStartupCost(0.000001);

    coefficients.setUnwindIncrementalCost(0.03 * ms);  // TODO: not yet calibrated
    coefficients.setUnwindStartupCost(0.000001);

    // TODO: not yet calibrated, should be at least as expensive as a filter
    coefficients.setBinaryJoinIncrementalCost(0.2 * ms);
    coefficients.setBinaryJoinStartupCost(0.000001);

    coefficients.setHashJoinIncrementalCost(0.05 * ms);  // TODO: not yet calibrated
    coefficients.setHashJoinStartupCost(0.000001);

    coefficients.setMergeJoinIncrementalCost(0.02 * ms);  // TODO: not yet calibrated
    coefficients.setMergeJoinStartupCost(0.000001);

    coefficients.setUniqueIncrementalCost(0.7 * ms);
    coefficients.setUniqueStartupCost(0.000001);

    // TODO: implement collation cost that depends on number and size of sorted fields
    // Based on a mix of int and str(64) fields:
    //  1 sort field:  sort_cost(N) = 1.0/10 * N * log(N)
    //  5 sort fields: sort_cost(N) = 2.5/10 * N * log(N)
    // 10 sort fields: sort_cost(N) = 3.0/10 * N * log(N)
    // field_cost_coeff(F) ~ 0.75 + 0.2 * F
    coefficients.setCollationIncrementalCost(2.5 * ms);  // 5 fields avg
    coefficients.setCollationStartupCost(0.000001);

    coefficients.setCollationWithLimitIncrementalCost(1.0 * ms);  // TODO: not yet calibrated
    coefficients.setCollationWithLimitStartupCost(0.000001);

    coefficients.setUnionIncrementalCost(0.02 * ms);
    coefficients.setUnionStartupCost(0.000001);

    coefficients.setExchangeIncrementalCost(0.1 * ms);  // TODO: not yet calibrated
    coefficients.setExchangeStartupCost(0.000001);

    coefficients.setLimitSkipIncrementalCost(0.0 * ms);  // TODO: not yet calibrated
    coefficients.setLimitSkipStartupCost(0.000001);
}
}  // namespace

CostModelManager::CostModelManager() {
    initializeCoefficients(_coefficients);
}

CostModelCoefficients CostModelManager::getCoefficients() const {
    std::shared_lock rLock(_mutex);  // NOLINT

    return _coefficients;
}

CostModelCoefficients CostModelManager::getDefaultCoefficients() {
    CostModelCoefficients defaultCoefs;

    initializeCoefficients(defaultCoefs);

    return defaultCoefs;
}

void CostModelManager::updateCostModelCoefficients(const BSONObj& overrides) {
    CostModelCoefficients newCoefs;
    if (overrides.isEmpty()) {
        initializeCoefficients(newCoefs);
    } else {
        auto coefsObj = _coefficients.toBSON();
        newCoefs = CostModelCoefficients::parse(IDLParserContext{"CostModelCoefficients"},
                                                coefsObj.addFields(overrides));
    }

    stdx::unique_lock wLock(_mutex);
    _coefficients = std::move(newCoefs);
}
}  // namespace mongo::cost_model

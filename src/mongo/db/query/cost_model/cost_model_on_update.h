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

#pragma once

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/sbe_plan_cache_on_parameter_change.h"
#include "mongo/db/service_context.h"

namespace mongo::cost_model {

Status updateCostCoefficients();

/**
 * On-update hook to update the cost coefficients in 'CostModelManager' when cost coefficients are
 * updated.
 */
constexpr inline auto updateCostCoefficientsOnUpdate = [](auto&&) {
    auto status = updateCostCoefficients();
    if (status != Status::OK()) {
        return status;
    }
    return plan_cache_util::clearSbeCacheOnParameterChangeHelper();
};

class OnCoefficientsChangeUpdater {
public:
    virtual ~OnCoefficientsChangeUpdater() = default;

    /**
     * This function should update the cost coefficients stored in 'CostModelManager'.
     */
    virtual void updateCoefficients(ServiceContext* serviceCtx, const BSONObj& overrides) = 0;
};

/**
 * Decorated accessor to the 'OnCoefficientsChangeUpdater' stored in 'ServiceContext'.
 */
extern const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnCoefficientsChangeUpdater>>
    onCoefficientsChangeUpdater;
}  // namespace mongo::cost_model

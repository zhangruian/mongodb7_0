/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/expression.h"

namespace mongo::stage_builder {
/**
 * This function generates an SBE plan stage tree implementing a filter expression represented by
 * 'root'. The 'stage' parameter provides the input subtree to build on top of. The 'inputSlotIn'
 * parameter specifies the input slot the filter should use. The 'relevantSlotsIn' parameter
 * specifies the slots produced by the 'stage' subtree that must remain visible to consumers of
 * the tree returned by this function.
 */
std::unique_ptr<sbe::PlanStage> generateFilter(OperationContext* opCtx,
                                               const MatchExpression* root,
                                               std::unique_ptr<sbe::PlanStage> stage,
                                               sbe::value::SlotIdGenerator* slotIdGenerator,
                                               sbe::value::FrameIdGenerator* frameIdGenerator,
                                               sbe::value::SlotId inputSlotIn,
                                               sbe::RuntimeEnvironment* env,
                                               sbe::value::SlotVector relevantSlotsIn,
                                               PlanNodeId planNodeId);

}  // namespace mongo::stage_builder

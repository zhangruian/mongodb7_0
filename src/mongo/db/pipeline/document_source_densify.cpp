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

#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/util/visit_helper.h"
namespace mongo {

namespace document_source_densify {
// TODO SERVER-57334 Translation logic goes here.
}  // namespace document_source_densify

DocumentSourceInternalDensify::DocGenerator::DocGenerator(
    DocumentSourceInternalDensify::DensifyValueType min,
    DocumentSourceInternalDensify::DensifyValueType max,
    StepSpec step,
    FieldPath fieldName,
    Document includeFields,
    Document finalDoc)
    : _step(std::move(step)),
      _path(std::move(fieldName.fullPath())),
      _includeFields(std::move(includeFields)),
      _finalDoc(std::move(finalDoc)),
      _min(std::move(min)),
      _max(std::move(max)) {
    tassert(5733306,
            "DocGenerator cannot include field that is being densified",
            _includeFields.getNestedField(_path).missing());

    // Traverse the preserved fields document to make sure we're not going through an array.
    auto traverseDoc = _includeFields;
    auto pathLength = _path.getPathLength();
    for (size_t i = 0; i < pathLength; ++i) {
        auto curVal = traverseDoc.getField(_path.getFieldName(i));
        uassert(5733307, "$densify cannot generate fields nested inside arrays", !curVal.isArray());
        if (curVal.isObject()) {
            traverseDoc = curVal.getDocument();
        } else {
            // Can't write to a field that has a non-object value as a path-prefix, as that would
            // overwrite data. We should only have a non-object at the end of the path.
            uassert(5733308,
                    "$densify cannot overwrite non-object values with objects",
                    i == pathLength - 1 || curVal.missing());
            break;
        }
    }

    stdx::visit(
        visit_helper::Overloaded{
            [&](const double doubleMin) {
                tassert(5733304,
                        "DocGenerator all values must be same type",
                        stdx::holds_alternative<double>(_max) &&
                            stdx::holds_alternative<double>(_step.step));
                tassert(5733303,
                        "DocGenerator min must be lower or equal to max",
                        stdx::get<double>(_max) >= doubleMin);
                tassert(5733305,
                        "DocGenerator step must be positive",
                        stdx::get<double>(_step.step) > 0);
            },
            [&](const Date_t dateMin) {
                // TODO SERVER-57335 Support dates and check date min/max values.
                tasserted(5733300, "DocGenerator does not currently support dates");
            },
        },
        _min);
}

Document DocumentSourceInternalDensify::DocGenerator::getNextDocument() {
    tassert(5733301,
            "Called DocGenerator::getNextDocument() but generator is done",
            _state != GeneratorState::kDone);
    if (_state == GeneratorState::kReturningFinalDocument) {
        _state = GeneratorState::kDone;
        return _finalDoc;
    }
    // Assume all types have been checked at this point and we are in a valid state.
    Value valueToAdd;
    stdx::visit(
        visit_helper::Overloaded{
            [&](double doubleVal) {
                valueToAdd = Value(doubleVal);
                doubleVal += stdx::get<double>(_step.step);
                if (doubleVal > stdx::get<double>(_max)) {
                    _state = GeneratorState::kReturningFinalDocument;
                }
                _min = doubleVal;
            },
            [&](Date_t dateMin) {
                // TODO SERVER-57335 Support dates and check date min/max values.
                tasserted(5733302, "DocGenerator date support not yet implemented");
            },
        },
        _min);

    MutableDocument retDoc(_includeFields);
    retDoc.setNestedField(_path, valueToAdd);
    return retDoc.freeze();
}

bool DocumentSourceInternalDensify::DocGenerator::done() const {
    return _state == GeneratorState::kDone;
}

}  // namespace mongo

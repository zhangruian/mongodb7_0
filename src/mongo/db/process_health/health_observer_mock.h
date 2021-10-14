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
#pragma once

#include <functional>

#include "mongo/db/process_health/fault_facet_mock.h"
#include "mongo/db/process_health/health_observer_base.h"

namespace mongo {
namespace process_health {

/**
 * Mocked health observer is using a test callback to fetch the next
 * fault severity value every time the periodic check is invoked.
 */
class HealthObserverMock : public HealthObserverBase {
public:
    HealthObserverMock(FaultFacetType mockType,
                       ClockSource* clockSource,
                       TickSource* tickSource,
                       std::function<double()> getSeverityCallback)
        : HealthObserverBase(clockSource, tickSource),
          _mockType(mockType),
          _getSeverityCallback(getSeverityCallback) {}

    virtual ~HealthObserverMock() = default;

protected:
    FaultFacetType getType() const override {
        return _mockType;
    }

    Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) override {
        // Detects mocked severity and handles it.
        const double severity = _getSeverityCallback();

        auto completionPf = makePromiseFuture<HealthCheckStatus>();
        if (HealthCheckStatus::isResolved(severity)) {
            completionPf.promise.emplaceValue(HealthCheckStatus(getType()));
        } else {
            completionPf.promise.emplaceValue(
                HealthCheckStatus(getType(), severity, "failed", Milliseconds(0), Milliseconds(0)));
        }
        return std::move(completionPf.future);
    }

    HealthObserverIntensity getIntensity() override {
        return HealthObserverIntensity::kNonCritical;
    }

private:
    const FaultFacetType _mockType;
    std::function<double()> _getSeverityCallback;
};

}  // namespace process_health
}  // namespace mongo

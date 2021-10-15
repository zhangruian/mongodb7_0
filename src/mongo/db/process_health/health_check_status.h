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

#include <memory>
#include <ostream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace process_health {

/**
 * All fault types we support in this package.
 */
enum class FaultFacetType { kMock1 = 0, kMock2, kLdap };

/**
 * The class representing current status of an ongoing fault tracked by facet.
 */
class HealthCheckStatus {
public:
    static constexpr double kResolvedSeverity = 0;
    // The range for active fault is inclusive: [ 1, Inf ).
    static constexpr double kActiveFaultSeverity = 1.0;
    // We chose to subtract a small 'epsilon' value from 1.0 to
    // avoid rounding problems and be sure that severity of 1.0 is guaranteed to be an active fault.
    static constexpr double kActiveFaultSeverityEpsilon = 0.000001;

    HealthCheckStatus(FaultFacetType type,
                      double severity,
                      StringData description,
                      Milliseconds activeFaultDuration,
                      Milliseconds duration)
        : _type(type),
          _severity(severity),
          _description(description),
          _activeFaultDuration(activeFaultDuration),
          _duration(duration) {
        uassert(5949601,
                str::stream() << "Active fault duration " << _activeFaultDuration
                              << " cannot be longer than duration " << _duration,
                _duration >= _activeFaultDuration);
    }

    // Constructs a resolved status (no fault detected).
    HealthCheckStatus(FaultFacetType type) : _type(type), _severity(0), _description("resolved") {}

    HealthCheckStatus(const HealthCheckStatus&) = default;
    HealthCheckStatus& operator=(const HealthCheckStatus&) = default;

    /**
     * @return FaultFacetType of this status.
     */
    FaultFacetType getType() const {
        return _type;
    }

    /**
     * The fault severity value if any.
     *
     * @return Current fault severity. The expected values:
     *         0: Ok
     *         (0, 1.0): Transient fault condition
     *         [1.0, Inf): Active fault condition
     */
    double getSeverity() const {
        return _severity;
    }

    /**
     * Gets the duration of an active fault, if any.
     * This is the time from the moment the severity reached the 1.0 value
     * and stayed on or above 1.0.
     *
     * Note: each time the severity drops below 1.0 the duration is reset.
     */
    Milliseconds getActiveFaultDuration() const {
        return _activeFaultDuration;
    }

    /**
     * @return duration of the fault facet or fault from the moment it was created.
     */
    Milliseconds getDuration() const {
        return _duration;
    }

    void appendDescription(BSONObjBuilder* builder) const;

    BSONObj toBSON() const;

    std::string toString() const;

    // Helpers for severity levels.

    static bool isResolved(double severity) {
        return severity <= kResolvedSeverity;
    }

    static bool isTransientFault(double severity) {
        return severity > kResolvedSeverity && severity < kActiveFaultSeverity;
    }

    static bool isActiveFault(double severity) {
        // Range is inclusive.
        return severity >= kActiveFaultSeverity - kActiveFaultSeverityEpsilon;
    }

private:
    friend std::ostream& operator<<(std::ostream&, const HealthCheckStatus&);
    friend StringBuilder& operator<<(StringBuilder& s, const HealthCheckStatus& hcs);

    const FaultFacetType _type;
    const double _severity;
    const std::string _description;
    const Milliseconds _activeFaultDuration;
    const Milliseconds _duration;
};

inline StringBuilder& operator<<(StringBuilder& s, const FaultFacetType& type) {
    switch (type) {
        case FaultFacetType::kMock1:
            return s << "kMock1"_sd;
        case FaultFacetType::kMock2:
            return s << "kMock2"_sd;
        default:
            return s << "Uknown"_sd;
    }
}

inline std::ostream& operator<<(std::ostream& os, const FaultFacetType& type) {
    StringBuilder sb;
    sb << type;
    os << sb.stringData();
    return os;
}

inline void HealthCheckStatus::appendDescription(BSONObjBuilder* builder) const {
    builder->append("type", _type);
    builder->append("description", _description);
    builder->append("severity", _severity);
    builder->append("activeFaultDuration", _activeFaultDuration.toString());
    builder->append("duration", _duration.toString());
}

inline StringBuilder& operator<<(StringBuilder& s, const HealthCheckStatus& hcs) {
    BSONObjBuilder bob;
    hcs.appendDescription(&bob);
    return s << bob.obj();
}

inline std::ostream& operator<<(std::ostream& os, const HealthCheckStatus& hcs) {
    BSONObjBuilder bob;
    hcs.appendDescription(&bob);
    return os << bob.obj();
}

inline BSONObj HealthCheckStatus::toBSON() const {
    BSONObjBuilder bob;
    appendDescription(&bob);
    return bob.obj();
}

inline std::string HealthCheckStatus::toString() const {
    BSONObjBuilder bob;
    appendDescription(&bob);
    return bob.obj().toString();
}

}  // namespace process_health
}  // namespace mongo

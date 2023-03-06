/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/metrics/cumulative_metrics_state_holder.h"

namespace mongo {

template <typename Base, typename AnyState>
class WithStateManagementForInstanceMetrics : public Base {
public:
    template <typename... Args>
    WithStateManagementForInstanceMetrics(Args&&... args) : Base{std::forward<Args>(args)...} {}

    AnyState getState() const {
        return _state.load();
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        Base::getTypedCumulativeMetrics()->template onStateTransition<T>(before, boost::none);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        setState(after);
        Base::getTypedCumulativeMetrics()->template onStateTransition<T>(boost::none, after);
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        setState(after);
        Base::getTypedCumulativeMetrics()->template onStateTransition<T>(before, after);
    }

protected:
    template <typename T>
    void setState(T state) {
        static_assert(std::is_assignable_v<AnyState, T>);
        _state.store(state);
    }

private:
    AtomicWord<AnyState> _state;
};

}  // namespace mongo

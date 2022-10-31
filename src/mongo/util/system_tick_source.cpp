/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/system_tick_source.h"

#include <chrono>  // NOLINT
#include <memory>

#include "mongo/util/tick_source.h"

namespace mongo {

std::unique_ptr<TickSource> makeSystemTickSource() {
    class Steady : public TickSource {
        using C = std::chrono::steady_clock;  // NOLINT
        Tick getTicksPerSecond() override {
            static_assert(C::period::num == 1, "Fractional frequency disallowed");
            return C::period::den;
        }
        Tick getTicks() override {
            return C::now().time_since_epoch().count();
        }
    };
    return std::make_unique<Steady>();
}

TickSource* globalSystemTickSource() {
    static const auto p = makeSystemTickSource().release();
    return p;
}

}  // namespace mongo

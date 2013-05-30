/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "accumulator.h"

#include "db/pipeline/value.h"

namespace mongo {

    void AccumulatorLast::processInternal(const Value& input) {
        /* always remember the last value seen */
        _last = input;
    }

    Value AccumulatorLast::getValue() const {
        return _last;
    }

    AccumulatorLast::AccumulatorLast() {}

    intrusive_ptr<Accumulator> AccumulatorLast::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<AccumulatorLast> pAccumulator(
            new AccumulatorLast());
        return pAccumulator;
    }

    const char *AccumulatorLast::getOpName() const {
        return "$last";
    }
}

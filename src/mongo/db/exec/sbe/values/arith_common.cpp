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

#include "mongo/db/exec/sbe/values/arith_common.h"

/**
These common operations - Addition, Subtraction and Multiplication - are used in both the VM and
constant folding in the optimizer. These methods are extensible for any computation with SBE values.
*/
namespace mongo::sbe::value {

/**
 * The addition operation used by genericArithmeticOp.
 */
struct Addition {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.add(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs + rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::add(lhs, rhs, &result);
        }
    }
};

/**
 * The subtraction operation used by genericArithmeticOp.
 */
struct Subtraction {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.subtract(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs - rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::sub(lhs, rhs, &result);
        }
    }
};

/**
 * The multiplication operation used by genericArithmeticOp.
 */
struct Multiplication {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.multiply(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs * rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::mul(lhs, rhs, &result);
        }
    }
};

/**
 * This is a simple arithmetic operation templated by the Op parameter. It supports operations on
 * standard numeric types and also operations on the Date type.
 */
template <typename Op>
std::tuple<bool, value::TypeTags, value::Value> genericArithmeticOp(value::TypeTags lhsTag,
                                                                    value::Value lhsValue,
                                                                    value::TypeTags rhsTag,
                                                                    value::Value rhsValue) {
    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                int32_t result;

                if (!Op::doOperation(numericCast<int32_t>(lhsTag, lhsValue),
                                     numericCast<int32_t>(rhsTag, rhsValue),
                                     result)) {
                    return {
                        false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
                }
                // The result does not fit into int32_t so fallthru to the wider type.
                [[fallthrough]];
            }
            case value::TypeTags::NumberInt64: {
                int64_t result;
                if (!Op::doOperation(numericCast<int64_t>(lhsTag, lhsValue),
                                     numericCast<int64_t>(rhsTag, rhsValue),
                                     result)) {
                    return {
                        false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
                }
                // The result does not fit into int64_t so fallthru to the wider type.
                [[fallthrough]];
            }
            case value::TypeTags::NumberDecimal: {
                Decimal128 result;
                Op::doOperation(numericCast<Decimal128>(lhsTag, lhsValue),
                                numericCast<Decimal128>(rhsTag, rhsValue),
                                result);
                auto [tag, val] = value::makeCopyDecimal(result);
                return {true, tag, val};
            }
            case value::TypeTags::NumberDouble: {
                double result;
                Op::doOperation(numericCast<double>(lhsTag, lhsValue),
                                numericCast<double>(rhsTag, rhsValue),
                                result);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (lhsTag == TypeTags::Date || rhsTag == TypeTags::Date) {
        if (isNumber(lhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    numericCast<int64_t>(lhsTag, lhsValue), bitcastTo<int64_t>(rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(result)};
            }
        } else if (isNumber(rhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), numericCast<int64_t>(rhsTag, rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(result)};
            }
        } else {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(lhsValue), result)) {
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
        }
        // We got here if the Date operation overflowed.
        uasserted(ErrorCodes::Overflow, "date overflow");
    }

    return {false, value::TypeTags::Nothing, 0};
}

std::tuple<bool, value::TypeTags, value::Value> genericAdd(value::TypeTags lhsTag,
                                                           value::Value lhsValue,
                                                           value::TypeTags rhsTag,
                                                           value::Value rhsValue) {
    return genericArithmeticOp<Addition>(lhsTag, lhsValue, rhsTag, rhsValue);
}

std::tuple<bool, value::TypeTags, value::Value> genericSub(value::TypeTags lhsTag,
                                                           value::Value lhsValue,
                                                           value::TypeTags rhsTag,
                                                           value::Value rhsValue) {
    return genericArithmeticOp<Subtraction>(lhsTag, lhsValue, rhsTag, rhsValue);
}

std::tuple<bool, value::TypeTags, value::Value> genericMul(value::TypeTags lhsTag,
                                                           value::Value lhsValue,
                                                           value::TypeTags rhsTag,
                                                           value::Value rhsValue) {
    return genericArithmeticOp<Multiplication>(lhsTag, lhsValue, rhsTag, rhsValue);
}

}  // namespace mongo::sbe::value

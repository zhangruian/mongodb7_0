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

#include "mongo/db/cst/c_node.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/hex.h"
#include "mongo/util/visit_helper.h"

#include <numeric>
#include <type_traits>

namespace mongo {
using namespace std::string_literals;
namespace {
auto tabs(int num) {
    std::string out;
    for (; num > 0; num--)
        out += "\t";
    return out;
}

auto printFieldname(const CNode::Fieldname& fieldname) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const KeyFieldname& key) -> std::string {
                return key_fieldname::toString[static_cast<std::underlying_type_t<KeyFieldname>>(
                    key)];
            },
            [](const UserFieldname& user) { return user; }},
        fieldname);
}

template <typename T>
auto printValue(const T& payload) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const CNode::ArrayChildren& children) { return "<Array>"s; },
            [](const CNode::ObjectChildren& children) { return "<Object>"s; },
            [](const KeyValue& value) {
                return "<KeyValue "s +
                    key_value::toString[static_cast<std::underlying_type_t<KeyValue>>(value)] + ">";
            },
            [](const UserDouble& userDouble) {
                return "<UserDouble "s + std::to_string(userDouble) + ">";
            },
            [](const UserString& userString) { return "<UserString "s + userString + ">"; },
            [](const UserBinary& userBinary) {
                return "<UserBinary "s + typeName(userBinary.type) + ", " +
                    toHex(userBinary.data, userBinary.length) + ">";
            },
            [](const UserUndefined& userUndefined) { return "<UserUndefined>"s; },
            [](const UserObjectId& userObjectId) {
                return "<UserObjectId "s + userObjectId.toString() + ">";
            },
            [](const UserBoolean& userBoolean) {
                return "<UserBoolean "s + std::to_string(userBoolean) + ">";
            },
            [](const UserDate& userDate) {
                return "<UserDate "s +
                    [&] {
                        if (auto string = TimeZoneDatabase::utcZone().formatDate(
                                "%Y-%m-%dT%H:%M:%S.%LZ", userDate);
                            string.isOK())
                            return string.getValue();
                        else
                            return "illegal date"s;
                    }() +
                    ">";
            },
            [](const UserNull& userNull) { return "<UserNull>"s; },
            [](const UserRegex& userRegex) {
                return "<UserRegex "s + "/" + userRegex.pattern + "/" + userRegex.flags + ">";
            },
            [](const UserDBPointer& userDBPointer) {
                return "<UserDBPointer "s + userDBPointer.ns + ", " + userDBPointer.oid.toString() +
                    ">";
            },
            [](const UserJavascript& userJavascript) {
                return "<UserJavascript "s + userJavascript.code + ">";
            },
            [](const UserSymbol& userSymbol) { return "<UserSymbol "s + userSymbol.symbol + ">"; },
            [](const UserJavascriptWithScope& userJavascriptWithScope) {
                return "<UserJavascriptWithScope "s + userJavascriptWithScope.code + ", ";
                userJavascriptWithScope.scope.toString() + ">";
            },
            [](const UserInt& userInt) { return "<UserInt "s + std::to_string(userInt) + ">"; },
            [](const UserTimestamp& userTimestamp) {
                return "<UserTimestamp "s + userTimestamp.toString() + ">";
            },
            [](const UserLong& userLong) { return "<UserLong "s + std::to_string(userLong) + ">"; },
            [](const UserDecimal& userDecimal) {
                return "<UserDecimal "s + userDecimal.toString() + ">";
            },
            [](const UserMinKey& userMinKey) { return "<UserMinKey>"s; },
            [](const UserMaxKey& userMaxKey) { return "<UserMaxKey>"s; }},
        payload);
}

}  // namespace

std::string CNode::toStringHelper(int numTabs) const {
    return stdx::visit(
        visit_helper::Overloaded{
            [numTabs](const ArrayChildren& children) {
                return std::accumulate(children.cbegin(),
                                       children.cend(),
                                       tabs(numTabs) + "[\n",
                                       [numTabs](auto&& string, auto&& child) {
                                           return string + child.toStringHelper(numTabs + 1) + "\n";
                                       }) +
                    tabs(numTabs) + "]";
            },
            [numTabs](const ObjectChildren& children) {
                return std::accumulate(children.cbegin(),
                                       children.cend(),
                                       tabs(numTabs) + "{\n",
                                       [numTabs](auto&& string, auto&& childpair) {
                                           return string + tabs(numTabs) +
                                               printFieldname(childpair.first) + " :\n" +
                                               childpair.second.toStringHelper(numTabs + 1) + "\n";
                                       }) +
                    tabs(numTabs) + "}";
            },
            [this, numTabs](auto&&) { return tabs(numTabs) + printValue(payload); }},
        payload);
}

std::pair<BSONObj, bool> CNode::toBsonWithArrayIndicator() const {
    return stdx::visit(
        visit_helper::Overloaded{
            // Build an array which will lose its identity and appear as a BSONObj
            [](const ArrayChildren& children) {
                return std::pair{
                    std::accumulate(
                        children.cbegin(),
                        children.cend(),
                        BSONObj{},
                        [fieldCount = 0u](auto&& bson, auto&& child) mutable {
                            // This is a non-compound field. pull the BSONElement out of it's
                            // BSONObj shell and add it.
                            if (auto [childBson, isArray] = child.toBsonWithArrayIndicator();
                                childBson.nFields() > 0 &&
                                childBson.firstElementFieldNameStringData() == "")
                                return bson.addField(
                                    childBson
                                        .replaceFieldNames(BSON(std::to_string(fieldCount++) << ""))
                                        .firstElement());
                            // This field is an array. Reconstruct with BSONArray and add it.
                            else if (isArray)
                                return bson.addField(
                                    BSON(std::to_string(fieldCount++) << BSONArray{childBson})
                                        .firstElement());
                            // This field is an object. Add it directly.
                            else
                                return bson.addField(
                                    BSON(std::to_string(fieldCount++) << childBson).firstElement());
                        }),
                    true};
            },
            // Build an object in a BSONObj.
            [](const ObjectChildren& children) {
                return std::pair{
                    std::accumulate(
                        children.cbegin(),
                        children.cend(),
                        BSONObj{},
                        [](auto&& bson, auto&& childPair) {
                            // This is a non-compound field. pull the BSONElement out of it's
                            // BSONObj shell and add it.
                            if (auto [childBson, isArray] =
                                    childPair.second.toBsonWithArrayIndicator();
                                childBson.nFields() > 0 &&
                                childBson.firstElementFieldNameStringData() == "")
                                return bson.addField(childBson
                                                         .replaceFieldNames(BSON(
                                                             printFieldname(childPair.first) << ""))
                                                         .firstElement());
                            // This field is an array. Reconstruct with BSONArray and add it.
                            else if (isArray)
                                return bson.addField(
                                    BSON(printFieldname(childPair.first) << BSONArray{childBson})
                                        .firstElement());
                            // This field is an object. Add it directly.
                            else
                                return bson.addField(
                                    BSON(printFieldname(childPair.first) << childBson)
                                        .firstElement());
                        }),
                    false};
            },
            // Build a non-compound field in a BSONObj shell.
            [this](auto&&) {
                return std::pair{BSON("" << printValue(payload)), false};
            }},
        payload);
}

}  // namespace mongo

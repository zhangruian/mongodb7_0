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

#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/timeseries_dotted_path_support.h"

#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/ctype.h"

namespace mongo {
namespace timeseries {
namespace dotted_path_support {

namespace {

boost::optional<std::pair<StringData, StringData>> _splitPath(StringData path) {
    size_t idx = path.find('.');
    if (idx == std::string::npos) {
        return boost::none;
    }

    StringData left = path.substr(0, idx);
    StringData next = path.substr(idx + 1, path.size());

    return std::make_pair(left, next);
}

template <typename BSONElementColl>
void _extractAllElementsAlongBucketPath(const BSONObj& obj,
                                        StringData path,
                                        BSONElementColl& elements,
                                        bool expandArrayOnTrailingField,
                                        BSONDepthIndex depth,
                                        MultikeyComponents* arrayComponents) {
    auto handleElement = [&](BSONElement elem, StringData path) -> void {
        if (elem.eoo()) {
            size_t idx = path.find('.');
            if (idx != std::string::npos) {
                invariant(depth != std::numeric_limits<BSONDepthIndex>::max());
                StringData left = path.substr(0, idx);
                StringData next = path.substr(idx + 1, path.size());

                BSONElement e = obj.getField(left);

                if (e.type() == Object) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       next,
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                } else if (e.type() == Array) {
                    bool allDigits = false;
                    if (next.size() > 0 && ctype::isDigit(next[0])) {
                        unsigned temp = 1;
                        while (temp < next.size() && ctype::isDigit(next[temp]))
                            temp++;
                        allDigits = temp == next.size() || next[temp] == '.';
                    }
                    if (allDigits) {
                        _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                           next,
                                                           elements,
                                                           expandArrayOnTrailingField,
                                                           depth + 1,
                                                           arrayComponents);
                    } else {
                        BSONObjIterator i(e.embeddedObject());
                        while (i.more()) {
                            BSONElement e2 = i.next();
                            if (e2.type() == Object || e2.type() == Array)
                                _extractAllElementsAlongBucketPath(e2.embeddedObject(),
                                                                   next,
                                                                   elements,
                                                                   expandArrayOnTrailingField,
                                                                   depth + 1,
                                                                   arrayComponents);
                        }
                        if (arrayComponents) {
                            arrayComponents->insert(depth);
                        }
                    }
                } else {
                    // do nothing: no match
                }
            }
        } else {
            if (elem.type() == Array && expandArrayOnTrailingField) {
                BSONObjIterator i(elem.embeddedObject());
                while (i.more()) {
                    elements.insert(i.next());
                }
                if (arrayComponents) {
                    arrayComponents->insert(depth);
                }
            } else {
                elements.insert(elem);
            }
        }
    };

    switch (depth) {
        case 0:
        case 1: {
            if (auto res = _splitPath(path)) {
                auto& [left, next] = *res;
                BSONElement e = obj.getField(left);
                if (e.type() == Object && (depth > 0 || left == timeseries::kBucketDataFieldName)) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       next,
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                }
            } else {
                BSONElement e = obj.getField(path);
                if (Object == e.type()) {
                    _extractAllElementsAlongBucketPath(e.embeddedObject(),
                                                       StringData(),
                                                       elements,
                                                       expandArrayOnTrailingField,
                                                       depth + 1,
                                                       arrayComponents);
                }
            }
            break;
        }
        case 2: {
            // Unbucketing magic happens here.
            for (const BSONElement& e : obj) {
                std::string subPath = e.fieldName();
                if (!path.empty()) {
                    subPath.append("." + path);
                }
                BSONElement sub = obj.getField(subPath);
                handleElement(sub, subPath);
            }
            break;
        }
        default: {
            BSONElement e = obj.getField(path);
            handleElement(e, path);
            break;
        }
    }
}


bool _haveArrayAlongBucketDataPath(const BSONObj& obj, StringData path, BSONDepthIndex depth);

bool _handleElementForHaveArrayAlongBucketDataPath(const BSONObj& obj,
                                                   BSONElement elem,
                                                   StringData path,
                                                   BSONDepthIndex depth) {
    if (elem.eoo()) {
        size_t idx = path.find('.');
        if (idx != std::string::npos) {
            tassert(5930502,
                    "BSON depth too great",
                    depth != std::numeric_limits<BSONDepthIndex>::max());
            StringData left = path.substr(0, idx);
            StringData next = path.substr(idx + 1, path.size());

            BSONElement e = obj.getField(left);

            if (e.type() == Object) {
                return _haveArrayAlongBucketDataPath(e.embeddedObject(), next, depth + 1);
            } else if (e.type() == Array) {
                return true;
            } else {
                // do nothing: no match
            }
        }
    } else {
        if (elem.type() == Array) {
            return true;
        }
    }

    return false;
}

bool _haveArrayAlongBucketDataPath(const BSONObj& obj, StringData path, BSONDepthIndex depth) {
    switch (depth) {
        case 0:
        case 1: {
            if (auto res = _splitPath(path)) {
                auto& [left, next] = *res;
                BSONElement e = obj.getField(left);
                if (e.type() == Object && (depth > 0 || left == timeseries::kBucketDataFieldName)) {
                    return _haveArrayAlongBucketDataPath(e.embeddedObject(), next, depth + 1);
                }
            } else {
                BSONElement e = obj.getField(path);
                if (Object == e.type()) {
                    return _haveArrayAlongBucketDataPath(
                        e.embeddedObject(), StringData(), depth + 1);
                }
            }
            return false;
        }
        case 2: {
            // Unbucketing magic happens here.
            for (const BSONElement& e : obj) {
                std::string subPath = e.fieldName();
                if (!path.empty()) {
                    subPath.append("." + path);
                }
                BSONElement sub = obj.getField(subPath);
                const bool foundArray =
                    _handleElementForHaveArrayAlongBucketDataPath(obj, sub, subPath, depth);
                if (foundArray) {
                    return foundArray;
                }
            }
            return false;
        }
        default: {
            BSONElement e = obj.getField(path);
            return _handleElementForHaveArrayAlongBucketDataPath(obj, e, path, depth);
        }
    }
}

std::pair<BSONElement, BSONElement> _getLiteralFields(const BSONObj& min,
                                                      const BSONObj& max,
                                                      StringData field) {
    return std::make_pair(min.getField(field), max.getField(field));
}


Decision _controlTypesIndicateArrayData(const BSONElement& min,
                                        const BSONElement& max,
                                        bool terminal) {
    if (min.type() <= BSONType::Array && max.type() >= BSONType::Array) {
        return (min.type() == BSONType::Array || max.type() == BSONType::Array) ? Decision::Yes
                                                                                : Decision::Maybe;
    }

    if (!terminal && (min.type() == BSONType::Object || max.type() == BSONType::Object)) {
        return Decision::Undecided;
    }

    return Decision::No;
}

std::tuple<BSONElement, BSONElement, std::string> _getNextFields(const BSONObj& min,
                                                                 const BSONObj& max,
                                                                 StringData field) {
    if (auto res = _splitPath(field)) {
        auto& [left, next] = *res;
        return std::make_tuple(min.getField(left), max.getField(left), next.toString());
    }
    return std::make_tuple(BSONElement(), BSONElement(), std::string());
}

Decision _fieldContainsArrayData(const BSONObj& maxObj, StringData field) {
    // When we get here, we know that some prefix value on the control.min path was a non-object
    // type < Object. We can also assume that our parent was an Object.

    auto e = maxObj.getField(field);
    if (!e.eoo()) {
        if (e.type() == BSONType::Array) {
            return Decision::Yes;
        } else if (e.type() > BSONType::Array) {
            return Decision::Maybe;
        }
        return Decision::No;
    }

    if (auto res = _splitPath(field)) {
        auto& [left, next] = *res;
        e = maxObj.getField(left);

        if (e.type() >= BSONType::Array) {
            return e.type() == BSONType::Array ? Decision::Yes : Decision::Maybe;
        } else if (e.type() < BSONType::Object) {
            return Decision::No;
        }
        tassert(5993301, "Expecting a sub-object.", e.isABSONObj());
        return _fieldContainsArrayData(e.embeddedObject(), next);
    }

    // Field is eoo(). Use parent type Object to draw conclusion conclusion.
    return Decision::No;
}

Decision _fieldContainsArrayData(const BSONObj& min, const BSONObj& max, StringData field) {
    // Invariants to consider coming into this function.
    //  1. min an max are both known to be objects
    //  2. field is some (possibly whole) suffix of the indexed user field (e.g. if the user defines
    //     an index on "a.b.c", then field is "c", "b.c", or "a.b.c", but does not include the
    //     "control..." prefix for the index defined on the bucket collection).
    //  3. Every field in the prefix corresponding to field is an object. That is, if the user index
    //     is defined on "a.b.c" and we have "c", then "control", "control.min", "control.max",
    //     "control.min.a", "control.max.a", "control.min.a.b", and "control.max.a.b" are all
    //     objects.

    // Let's decide whether we are looking at the terminal field on the dotted path, or if we might
    // need to unpack sub-objects.
    const bool terminal = std::string::npos == field.find('.');

    // First lets try to use the field name literally (i.e. treat it as terminal, even if it has an
    // internal dot).
    auto [minLit, maxLit] = _getLiteralFields(min, max, field);
    tassert(5993302, "Malformed control summary for bucket", minLit.eoo() == maxLit.eoo());
    if (!minLit.eoo() /* => !maxLit.eoo()*/) {
        return _controlTypesIndicateArrayData(minLit, maxLit, terminal);
    } else if (terminal) {
        // Nothing further to evaluate, the field is missing from min and max, and thus from all
        // measurements in this bucket.
        return Decision::No;
    }

    auto [minEl, maxEl, nextField] = _getNextFields(min, max, field);
    invariant(terminal == nextField.empty());
    auto decision = _controlTypesIndicateArrayData(minEl, maxEl, terminal);
    if (decision != Decision::Undecided) {
        return decision;
    }

    // Since we are undecided, at least one of minEl and maxEl must be of type object. We know
    // minEl.type() <= maxEl.type(), and we know that if minEl.type == Object and maxEl.type() >
    // Object, then we would have gotten a Yes decision above, so it must be the case that
    // minEl.type() <= Object and maxEl.type() == Object.
    if (!minEl.isABSONObj()) {
        return _fieldContainsArrayData(maxEl.embeddedObject(), nextField);
    }

    // We preserve the invariants mentioned above for the recursive call, where both are objects.
    return _fieldContainsArrayData(minEl.embeddedObject(), maxEl.embeddedObject(), nextField);
}

}  // namespace

void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementSet& elements,
                                       bool expandArrayOnTrailingField,
                                       MultikeyComponents* arrayComponents) {
    constexpr BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongBucketPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementMultiSet& elements,
                                       bool expandArrayOnTrailingField,
                                       MultikeyComponents* arrayComponents) {
    constexpr BSONDepthIndex initialDepth = 0;
    _extractAllElementsAlongBucketPath(
        obj, path, elements, expandArrayOnTrailingField, initialDepth, arrayComponents);
}

bool haveArrayAlongBucketDataPath(const BSONObj& bucketObj, StringData path) {
    // Shortcut: if we aren't checking a `data.` path, then we don't care.
    if (!path.startsWith(timeseries::kDataFieldNamePrefix)) {
        return false;
    }

    constexpr BSONDepthIndex initialDepth = 0;
    return _haveArrayAlongBucketDataPath(bucketObj, path, initialDepth);
}

std::ostream& operator<<(std::ostream& s, const Decision& i) {
    switch (i) {
        case Decision::Yes:
            s << "Yes";
            break;
        case Decision::Maybe:
            s << "Maybe";
            break;
        case Decision::No:
            s << "No";
            break;
        case Decision::Undecided:
            s << "Undecided";
            break;
    }
    return s;
}

Decision fieldContainsArrayData(const BSONObj& bucketObj, StringData userField) {
    // In general, we are searching for an array, or for a type mismatch somewhere along the path
    // in the summary fields, such that it can hide array values in the data field. For example if
    // we are interested in the user field 'a.b', we will examine the paths 'control.min.a.b' and
    // 'control.max.a.b'. If we are able to determine that along both paths, 'a' corresponds to an
    // object and 'a.b.' to a double, then we know that there can be no array data between them, as
    // an array compares greater than an object or a double, and would be reflected in the
    // 'control.max' field somewhere. Similarly, if we find that 'control.min.a' is a double and
    // 'control.max.a' is a bool, then there may be an array hidden between them. There are more
    // complex cases where one path yields a sub-object and the other contains a scalar type, but
    // the overall concept remains the same.

    BSONElement control = bucketObj.getField(timeseries::kBucketControlFieldName);
    tassert(5993303,
            "Expecting bucket object to have control field",
            !control.eoo() && control.isABSONObj());
    BSONObj controlObj = control.embeddedObject();

    BSONElement min = controlObj.getField(timeseries::kBucketControlMinFieldName);
    tassert(5993304,
            "Expecting bucket object to have control.min field",
            !min.eoo() && min.isABSONObj());
    BSONElement max = controlObj.getField(timeseries::kBucketControlMaxFieldName);
    tassert(5993305,
            "Expecting bucket object to have control.max field",
            !max.eoo() && max.isABSONObj());

    auto decision = _fieldContainsArrayData(min.embeddedObject(), max.embeddedObject(), userField);
    tassert(5993306, "Unable to make a decision", decision != Decision::Undecided);
    return decision;
}

}  // namespace dotted_path_support
}  // namespace timeseries
}  // namespace mongo

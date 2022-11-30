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

#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::stats {
namespace {
TypeCounts mapStatsTypeCountToTypeCounts(std::vector<TypeTag> tc) {
    TypeCounts out;
    for (const auto& t : tc) {
        out.emplace(deserialize(t.getTypeName().toString()), t.getCount());
    }
    return out;
}
}  // namespace

ArrayHistogram::ArrayHistogram() : ArrayHistogram(ScalarHistogram(), {}) {}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar,
                               TypeCounts typeCounts,
                               ScalarHistogram arrayUnique,
                               ScalarHistogram arrayMin,
                               ScalarHistogram arrayMax,
                               TypeCounts arrayTypeCounts,
                               double emptyArrayCount,
                               double trueCount,
                               double falseCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(emptyArrayCount),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _arrayUnique(std::move(arrayUnique)),
      _arrayMin(std::move(arrayMin)),
      _arrayMax(std::move(arrayMax)),
      _arrayTypeCounts(std::move(arrayTypeCounts)) {
    invariant(isArray());
}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar,
                               TypeCounts typeCounts,
                               double trueCount,
                               double falseCount)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _emptyArrayCount(0.0),
      _trueCount(trueCount),
      _falseCount(falseCount),
      _arrayUnique(boost::none),
      _arrayMin(boost::none),
      _arrayMax(boost::none),
      _arrayTypeCounts(boost::none) {
    invariant(!isArray());
}

ArrayHistogram* ArrayHistogram::makeArrayHistogram(Statistics stats) {
    if (auto maybeArrayStats = stats.getArrayStatistics(); maybeArrayStats) {
        return new ArrayHistogram(stats.getScalarHistogram(),
                                  mapStatsTypeCountToTypeCounts(stats.getTypeCount()),
                                  maybeArrayStats->getUniqueHistogram(),
                                  maybeArrayStats->getMinHistogram(),
                                  maybeArrayStats->getMaxHistogram(),
                                  mapStatsTypeCountToTypeCounts(maybeArrayStats->getTypeCount()),
                                  stats.getTrueCount(),
                                  stats.getFalseCount());
    }

    // If we don't have ArrayStatistics available, we should construct a histogram with only scalar
    // fields.
    return new ArrayHistogram(stats.getScalarHistogram(),
                              mapStatsTypeCountToTypeCounts(stats.getTypeCount()),
                              stats.getTrueCount(),
                              stats.getFalseCount());
}

bool ArrayHistogram::isArray() const {
    return _arrayUnique && _arrayMin && _arrayMax && _arrayTypeCounts;
}

std::string typeCountsToString(const TypeCounts& typeCounts) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (auto [tag, count] : typeCounts) {
        if (!first)
            os << ", ";
        os << tag << ": " << count;
        first = false;
    }
    os << "}";
    return os.str();
}

std::string ArrayHistogram::toString() const {
    std::ostringstream os;
    os << "{\n";
    os << " scalar: " << _scalar.toString();
    os << ",\n typeCounts: " << typeCountsToString(_typeCounts);
    if (isArray()) {
        os << ",\n arrayUnique: " << _arrayUnique->toString();
        os << ",\n arrayMin: " << _arrayMin->toString();
        os << ",\n arrayMax: " << _arrayMax->toString();
        os << ",\n arrayTypeCounts: " << typeCountsToString(*_arrayTypeCounts);
    }
    os << "\n}\n";
    return os.str();
}

const ScalarHistogram& ArrayHistogram::getScalar() const {
    return _scalar;
}

const ScalarHistogram& ArrayHistogram::getArrayUnique() const {
    invariant(isArray());
    return *_arrayUnique;
}

const ScalarHistogram& ArrayHistogram::getArrayMin() const {
    invariant(isArray());
    return *_arrayMin;
}

const ScalarHistogram& ArrayHistogram::getArrayMax() const {
    invariant(isArray());
    return *_arrayMax;
}

const TypeCounts& ArrayHistogram::getTypeCounts() const {
    return _typeCounts;
}

const TypeCounts& ArrayHistogram::getArrayTypeCounts() const {
    invariant(isArray());
    return *_arrayTypeCounts;
}

double ArrayHistogram::getArrayCount() const {
    if (isArray()) {
        auto findArray = _typeCounts.find(sbe::value::TypeTags::Array);
        uassert(6979504,
                "Histogram with array data must have a total array count.",
                findArray != _typeCounts.end());
        double arrayCount = findArray->second;
        uassert(6979503, "Histogram with array data must have at least one array.", arrayCount > 0);
        return arrayCount;
    }
    return 0;
}

void serializeTypeCounts(const TypeCounts& typeCounts, BSONObjBuilder& bob) {
    BSONArrayBuilder typeCountBuilder(bob.subarrayStart("typeCount"));
    for (const auto& [sbeType, count] : typeCounts) {
        auto typeCount = BSON("typeName" << stats::serialize(sbeType) << "count" << count);
        typeCountBuilder.append(typeCount);
    }
    typeCountBuilder.doneFast();
}

BSONObj ArrayHistogram::serialize() const {
    BSONObjBuilder histogramBuilder;

    // Serialize boolean type counters.
    histogramBuilder.append("trueCount", getTrueCount());
    histogramBuilder.append("falseCount", getFalseCount());

    // Serialize empty array counts.
    histogramBuilder.appendNumber("emptyArrayCount", getEmptyArrayCount());

    // Serialize type counts.
    serializeTypeCounts(getTypeCounts(), histogramBuilder);

    // Serialize scalar histogram.
    histogramBuilder.append("scalarHistogram", getScalar().serialize());

    if (isArray()) {
        // Serialize array histograms and type counts.
        BSONObjBuilder arrayStatsBuilder(histogramBuilder.subobjStart("arrayStatistics"));
        arrayStatsBuilder.append("minHistogram", getArrayMin().serialize());
        arrayStatsBuilder.append("maxHistogram", getArrayMax().serialize());
        arrayStatsBuilder.append("uniqueHistogram", getArrayUnique().serialize());
        serializeTypeCounts(getArrayTypeCounts(), arrayStatsBuilder);
        arrayStatsBuilder.doneFast();
    }

    histogramBuilder.doneFast();
    return histogramBuilder.obj();
}

BSONObj makeStatistics(double documents, const ArrayHistogram& arrayHistogram) {
    BSONObjBuilder builder;
    builder.appendNumber("documents", documents);
    builder.appendElements(arrayHistogram.serialize());
    builder.doneFast();
    return builder.obj();
}

BSONObj makeStatsPath(StringData path, double documents, const ArrayHistogram& arrayHistogram) {
    BSONObjBuilder builder;
    builder.append("_id", path);
    builder.append("statistics", makeStatistics(documents, arrayHistogram));
    builder.doneFast();
    return builder.obj();
}

}  // namespace mongo::stats

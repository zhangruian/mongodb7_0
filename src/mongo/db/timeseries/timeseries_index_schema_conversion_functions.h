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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {

class OperationContext;
class TimeseriesOptions;

/**
 * Namespace for helper functions converting index spec schema between time-series collection and
 * underlying buckets collection.
 */
namespace timeseries {

/**
 * Maps the time-series collection index spec 'timeseriesIndexSpecBSON' to the index schema of the
 * underlying bucket collection using the information provided in 'timeseriesOptions'.
 *
 * Returns an error if the specified 'timeseriesKeyBSON' is invalid for the time-series collection.
 */
StatusWith<BSONObj> createBucketsIndexSpecFromTimeseriesIndexSpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON);

StatusWith<BSONObj> createBucketsShardKeySpecFromTimeseriesShardKeySpec(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& timeseriesIndexSpecBSON);

/**
 * Returns a time-series collection index spec equivalent to the given 'bucketsIndex' using the
 * time-series specifications provided in 'timeseriesOptions'. Returns boost::none if the
 * buckets index is not supported on a time-series collection.
 *
 * Copies and modifies the 'key' field of the buckets index, but otherwises copies all of the fields
 * over unaltered.
 */
boost::optional<BSONObj> createTimeseriesIndexFromBucketsIndex(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& bucketsIndex);

/**
 * Returns a list of time-series collection index specs equivalent to the given 'bucketsIndexSpecs'
 * using the time-series specifications provided in 'timeseriesOptions'. If any of the buckets
 * indexes is not supported on a time-series collection, it will be ommitted from the results.
 */
std::list<BSONObj> createTimeseriesIndexesFromBucketsIndexes(
    const TimeseriesOptions& timeseriesOptions, const std::list<BSONObj>& bucketsIndexes);

/**
 * Returns true if the 'bucketsIndex' is compatible for FCV downgrade.
 */
bool isBucketsIndexSpecCompatibleForDowngrade(const TimeseriesOptions& timeseriesOptions,
                                              const BSONObj& bucketsIndex);

}  // namespace timeseries
}  // namespace mongo

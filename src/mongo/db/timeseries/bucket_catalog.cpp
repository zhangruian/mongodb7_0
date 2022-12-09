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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/basic.h"

#include "mongo/db/timeseries/bucket_catalog.h"

#include <algorithm>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/utility/in_place_factory.hpp>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {
const auto getBucketCatalog = ServiceContext::declareDecoration<BucketCatalog>();
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeWriteConflict);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeReopeningBucket);
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationAfterStart);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesDirectModificationBeforeFinish);


uint8_t numDigits(uint32_t num) {
    uint8_t numDigits = 0;
    while (num) {
        num /= 10;
        ++numDigits;
    }
    return numDigits;
}

OperationId getOpId(OperationContext* opCtx,
                    BucketCatalog::CombineWithInsertsFromOtherClients combine) {
    switch (combine) {
        case BucketCatalog::CombineWithInsertsFromOtherClients::kAllow:
            return 0;
        case BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow:
            invariant(opCtx->getOpID());
            return opCtx->getOpID();
    }
    MONGO_UNREACHABLE;
}

BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime) {
    BSONObjBuilder builder;
    builder.append(timeField, roundedTime);
    return builder.obj();
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    OID oid = OID::gen();

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = timeseries::roundTimestampToGranularity(time, options);
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    oid.setTimestamp(roundedSeconds);

    // Now, if we stopped here we could end up with bucket OID collisions. Consider the case where
    // we have the granularity set to 'Hours'. This means we will round down to the nearest day, so
    // any bucket generated on the same machine on the same day will have the same timestamp portion
    // and unique instance portion of the OID. Only the increment will differ. Since we only use 3
    // bytes for the increment portion, we run a serious risk of overflow if we are generating lots
    // of buckets.
    //
    // To address this, we'll take the difference between the actual timestamp and the rounded
    // timestamp and add it to the instance portion of the OID to ensure we can't have a collision.
    // for timestamps generated on the same machine.
    //
    // This leaves open the possibility that in the case of step-down/step-up, we could get a
    // collision if the old primary and the new primary have unique instance bits that differ by
    // less than the maximum rounding difference. This is quite unlikely though, and can be resolved
    // by restarting the new primary. It remains an open question whether we can fix this in a
    // better way.
    // TODO (SERVER-61412): Avoid time-series bucket OID collisions after election
    auto instance = oid.getInstanceUnique();
    uint32_t sum = DataView(reinterpret_cast<char*>(instance.bytes)).read<uint32_t>(1) +
        (durationCount<Seconds>(time.toDurationSinceEpoch()) - roundedSeconds);
    DataView(reinterpret_cast<char*>(instance.bytes)).write<uint32_t>(sum, 1);
    oid.setInstanceUnique(instance);

    return {oid, roundedTime};
}

Status getTimeseriesBucketClearedError(const NamespaceString& ns, const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " for namespace " << ns
                          << " was cleared"};
}

/**
 * Caluculate the bucket max size constrained by the cache size and the cardinality of active
 * buckets.
 */
int32_t getCacheDerivedBucketMaxSize(StorageEngine* storageEngine, uint32_t workloadCardinality) {
    invariant(storageEngine);
    uint64_t storageCacheSize =
        static_cast<uint64_t>(storageEngine->getEngine()->getCacheSizeMB() * 1024 * 1024);

    if (!feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility) ||
        storageCacheSize == 0 || workloadCardinality == 0) {
        return INT_MAX;
    }

    uint64_t derivedMaxSize = storageCacheSize / (2 * workloadCardinality);
    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    return std::min(derivedMaxSize, intMax);
}

}  // namespace

void BucketCatalog::ExecutionStatsController::incNumBucketInserts(long long increment) {
    _collectionStats->numBucketInserts.fetchAndAddRelaxed(increment);
    _globalStats->numBucketInserts.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketUpdates(long long increment) {
    _collectionStats->numBucketUpdates.fetchAndAddRelaxed(increment);
    _globalStats->numBucketUpdates.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsOpenedDueToMetadata(
    long long increment) {
    _collectionStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToCount(long long increment) {
    _collectionStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToSchemaChange(
    long long increment) {
    _collectionStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToSize(long long increment) {
    _collectionStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToCachePressure(
    long long increment) {
    _collectionStats->numBucketsClosedDueToCachePressure.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToCachePressure.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToTimeForward(
    long long increment) {
    _collectionStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToTimeBackward(
    long long increment) {
    _collectionStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToMemoryThreshold(
    long long increment) {
    _collectionStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsClosedDueToReopening(
    long long increment) {
    _collectionStats->numBucketsClosedDueToReopening.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToReopening.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsArchivedDueToMemoryThreshold(
    long long increment) {
    _collectionStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsArchivedDueToTimeBackward(
    long long increment) {
    _collectionStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumCommits(long long increment) {
    _collectionStats->numCommits.fetchAndAddRelaxed(increment);
    _globalStats->numCommits.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumWaits(long long increment) {
    _collectionStats->numWaits.fetchAndAddRelaxed(increment);
    _globalStats->numWaits.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumMeasurementsCommitted(long long increment) {
    _collectionStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
    _globalStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsReopened(long long increment) {
    _collectionStats->numBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsReopened.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsKeptOpenDueToLargeMeasurements(
    long long increment) {
    _collectionStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketFetchesFailed(long long increment) {
    _collectionStats->numBucketFetchesFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketFetchesFailed.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketQueriesFailed(long long increment) {
    _collectionStats->numBucketQueriesFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketQueriesFailed.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsFetched(long long increment) {
    _collectionStats->numBucketsFetched.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsFetched.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketsQueried(long long increment) {
    _collectionStats->numBucketsQueried.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsQueried.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumBucketReopeningsFailed(long long increment) {
    _collectionStats->numBucketReopeningsFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailed.fetchAndAddRelaxed(increment);
}

void BucketCatalog::ExecutionStatsController::incNumDuplicateBucketsReopened(long long increment) {
    _collectionStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
}

BucketCatalog::BucketState& BucketCatalog::BucketState::setFlag(BucketStateFlag flag) {
    _state |= static_cast<decltype(_state)>(flag);
    return *this;
}

BucketCatalog::BucketState& BucketCatalog::BucketState::unsetFlag(BucketStateFlag flag) {
    _state &= ~static_cast<decltype(_state)>(flag);
    return *this;
}

BucketCatalog::BucketState& BucketCatalog::BucketState::reset() {
    _state = 0;
    return *this;
}

bool BucketCatalog::BucketState::isSet(BucketStateFlag flag) const {
    return _state & static_cast<decltype(_state)>(flag);
}


bool BucketCatalog::BucketState::isPrepared() const {
    constexpr decltype(_state) mask = static_cast<decltype(_state)>(BucketStateFlag::kPrepared);
    return _state & mask;
}

bool BucketCatalog::BucketState::conflictsWithReopening() const {
    constexpr decltype(_state) mask =
        static_cast<decltype(_state)>(BucketStateFlag::kPendingCompression) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingDirectWrite);
    return _state & mask;
}

bool BucketCatalog::BucketState::conflictsWithInsertion() const {
    constexpr decltype(_state) mask = static_cast<decltype(_state)>(BucketStateFlag::kCleared) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingCompression) |
        static_cast<decltype(_state)>(BucketStateFlag::kPendingDirectWrite);
    return _state & mask;
}

bool BucketCatalog::BucketState::operator==(const BucketState& other) const {
    return _state == other._state;
}

std::string BucketCatalog::BucketState::toString() const {
    str::stream str;
    str << "[";

    bool first = true;
    auto output = [&first, &str](std::string name) {
        if (first) {
            first = false;
        } else {
            str << ", ";
        }
        str << name;
    };

    if (isSet(BucketStateFlag::kPrepared)) {
        output("prepared");
    }

    if (isSet(BucketStateFlag::kCleared)) {
        output("cleared");
    }

    if (isSet(BucketStateFlag::kPendingCompression)) {
        output("pendingCompression");
    }

    if (isSet(BucketStateFlag::kPendingDirectWrite)) {
        output("pendingDirectWrite");
    }

    if (isSet(BucketStateFlag::kUntracked)) {
        output("untracked");
    }

    str << "]";
    return str;
}

BucketCatalog::BucketStateManager::BucketStateManager(Mutex* m) : _mutex(m), _era(0) {}

uint64_t BucketCatalog::BucketStateManager::getEra() {
    stdx::lock_guard lk{*_mutex};
    return _era;
}

uint64_t BucketCatalog::BucketStateManager::getEraAndIncrementCount() {
    stdx::lock_guard lk{*_mutex};
    _incrementEraCountHelper(_era);
    return _era;
}

void BucketCatalog::BucketStateManager::decrementCountForEra(uint64_t value) {
    stdx::lock_guard lk{*_mutex};
    _decrementEraCountHelper(value);
}

uint64_t BucketCatalog::BucketStateManager::getCountForEra(uint64_t value) {
    stdx::lock_guard lk{*_mutex};
    auto it = _countMap.find(value);
    if (it == _countMap.end()) {
        return 0;
    } else {
        return it->second;
    }
}

void BucketCatalog::BucketStateManager::clearSetOfBuckets(ShouldClearFn&& shouldClear) {
    stdx::lock_guard lk{*_mutex};
    _clearRegistry[++_era] = std::move(shouldClear);
}

uint64_t BucketCatalog::BucketStateManager::getClearOperationsCount() {
    return _clearRegistry.size();
}

void BucketCatalog::BucketStateManager::_decrementEraCountHelper(uint64_t era) {
    auto it = _countMap.find(era);
    invariant(it != _countMap.end());
    if (it->second == 1) {
        _countMap.erase(it);
        _cleanClearRegistry();
    } else {
        --it->second;
    }
}

void BucketCatalog::BucketStateManager::_incrementEraCountHelper(uint64_t era) {
    auto it = _countMap.find(era);
    if (it == _countMap.end()) {
        (_countMap)[era] = 1;
    } else {
        ++it->second;
    }
}

bool BucketCatalog::BucketStateManager::_isMemberOfClearedSet(WithLock catalogLock,
                                                              Bucket* bucket) {
    for (auto it = _clearRegistry.lower_bound(bucket->getEra() + 1); it != _clearRegistry.end();
         ++it) {
        if (it->second(bucket->ns())) {
            return true;
        }
    }
    if (bucket->getEra() != _era) {
        _decrementEraCountHelper(bucket->getEra());
        _incrementEraCountHelper(_era);
        bucket->setEra(_era);
    }

    return false;
}

boost::optional<BucketCatalog::BucketState>
BucketCatalog::BucketStateManager::_markIndividualBucketCleared(WithLock catalogLock,
                                                                const BucketId& bucketId) {
    return _changeBucketStateHelper(
        catalogLock,
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                return boost::none;
            }
            return input.value().setFlag(BucketStateFlag::kCleared);
        });
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::BucketStateManager::getBucketState(
    Bucket* bucket) {
    stdx::lock_guard catalogLock{*_mutex};
    // If the bucket has been cleared, we will set the bucket state accordingly to reflect that.
    if (_isMemberOfClearedSet(catalogLock, bucket)) {
        return _markIndividualBucketCleared(catalogLock, bucket->bucketId());
    }
    auto it = _bucketStates.find(bucket->bucketId());
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::BucketStateManager::getBucketState(
    const BucketId& bucketId) const {
    stdx::lock_guard catalogLock{*_mutex};
    auto it = _bucketStates.find(bucketId);
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::BucketStateManager::changeBucketState(
    Bucket* bucket, const StateChangeFn& change) {
    stdx::lock_guard catalogLock{*_mutex};
    if (_isMemberOfClearedSet(catalogLock, bucket)) {
        return _markIndividualBucketCleared(catalogLock, bucket->bucketId());
    }

    return _changeBucketStateHelper(catalogLock, bucket->bucketId(), change);
}

boost::optional<BucketCatalog::BucketState> BucketCatalog::BucketStateManager::changeBucketState(
    const BucketId& bucketId, const StateChangeFn& change) {
    stdx::lock_guard catalogLock{*_mutex};
    return _changeBucketStateHelper(catalogLock, bucketId, change);
}

void BucketCatalog::BucketStateManager::appendStats(BSONObjBuilder* base) const {
    stdx::lock_guard catalogLock{*_mutex};

    BSONObjBuilder builder{base->subobjStart("stateManagement")};

    builder.appendNumber("bucketsManaged", static_cast<long long>(_bucketStates.size()));
    builder.appendNumber("currentEra", static_cast<long long>(_era));
    builder.appendNumber("erasWithRemainingBuckets", static_cast<long long>(_countMap.size()));
    builder.appendNumber("trackedClearOperations", static_cast<long long>(_clearRegistry.size()));
}

boost::optional<BucketCatalog::BucketState>
BucketCatalog::BucketStateManager::_changeBucketStateHelper(WithLock catalogLock,
                                                            const BucketId& bucketId,
                                                            const StateChangeFn& change) {
    auto it = _bucketStates.find(bucketId);
    const boost::optional<BucketState> initial =
        (it == _bucketStates.end()) ? boost::none : boost::make_optional(it->second);
    const boost::optional<BucketState> target = change(initial, _era);

    // If we are initiating or finishing a direct write, we need to advance the era. This allows us
    // to synchronize with reopening attempts that do not directly observe a state with the
    // kPendingDirectWrite flag set, but which nevertheless may be trying to reopen a stale bucket.
    if ((target.has_value() && target.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!initial.has_value() || !initial.value().isSet(BucketStateFlag::kPendingDirectWrite))) ||
        (initial.has_value() && initial.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!target.has_value() || !target.value().isSet(BucketStateFlag::kPendingDirectWrite)))) {
        ++_era;
    }

    // If initial and target are not both set, then we are either initializing or erasing the state.
    if (!target.has_value()) {
        if (initial.has_value()) {
            _bucketStates.erase(it);
        }
        return boost::none;
    } else if (!initial.has_value()) {
        _bucketStates.emplace(bucketId, target.value());
        return target;
    }

    // At this point we can now assume that both initial and target are set.

    // We cannot prepare a bucket that isn't eligible for insertions. We expect to attempt this when
    // we try to prepare a batch on a bucket that's been recently cleared.
    if (!initial.value().isPrepared() && target.value().isPrepared() &&
        initial.value().conflictsWithInsertion()) {
        return initial;
    }

    // We cannot transition from a prepared state to pending compression, as that would indicate a
    // programmer error.
    invariant(!initial.value().isPrepared() ||
              !target.value().isSet(BucketStateFlag::kPendingCompression));

    it->second = target.value();

    return target;
}

void BucketCatalog::BucketStateManager::_cleanClearRegistry() {
    // An edge case occurs when the count map is empty. In this case, we can clean the whole clear
    // registry.
    if (_countMap.begin() == _countMap.end()) {
        _clearRegistry.erase(_clearRegistry.begin(), _clearRegistry.end());
        return;
    }

    uint64_t smallestEra = _countMap.begin()->first;
    auto endIt = upper_bound(_clearRegistry.begin(),
                             _clearRegistry.end(),
                             smallestEra,
                             [](uint64_t val, auto kv) { return val < kv.first; });

    _clearRegistry.erase(_clearRegistry.begin(), endIt);
}

BucketCatalog::Bucket::Bucket(const BucketId& bucketId,
                              StripeNumber stripe,
                              BucketKey::Hash keyHash,
                              BucketStateManager* bucketStateManager)
    : _lastCheckedEra(bucketStateManager->getEraAndIncrementCount()),
      _bucketStateManager(bucketStateManager),
      _bucketId(bucketId),
      _stripe(stripe),
      _keyHash(keyHash) {}

BucketCatalog::Bucket::~Bucket() {
    _bucketStateManager->decrementCountForEra(getEra());
}

uint64_t BucketCatalog::Bucket::getEra() const {
    return _lastCheckedEra;
}

void BucketCatalog::Bucket::setEra(uint64_t era) {
    _lastCheckedEra = era;
}

const BucketCatalog::BucketId& BucketCatalog::Bucket::bucketId() const {
    return _bucketId;
}

const OID& BucketCatalog::Bucket::oid() const {
    return _bucketId.oid;
}

const NamespaceString& BucketCatalog::Bucket::ns() const {
    return _bucketId.ns;
}

BucketCatalog::StripeNumber BucketCatalog::Bucket::stripe() const {
    return _stripe;
}

BucketCatalog::BucketKey::Hash BucketCatalog::Bucket::keyHash() const {
    return _keyHash;
}

Date_t BucketCatalog::Bucket::getTime() const {
    return _minTime;
}

StringData BucketCatalog::Bucket::getTimeField() {
    return _timeField;
}

bool BucketCatalog::Bucket::allCommitted() const {
    return _batches.empty() && !_preparedBatch;
}

uint32_t BucketCatalog::Bucket::numMeasurements() const {
    return _numMeasurements;
}

void BucketCatalog::Bucket::setRolloverAction(RolloverAction action) {
    _rolloverAction = action;
}

bool BucketCatalog::Bucket::schemaIncompatible(const BSONObj& input,
                                               boost::optional<StringData> metaField,
                                               const StringData::ComparatorInterface* comparator) {
    auto result = _schema.update(input, metaField, comparator);
    return (result == timeseries::Schema::UpdateStatus::Failed);
}

void BucketCatalog::Bucket::_calculateBucketFieldsAndSizeChange(
    const BSONObj& doc,
    boost::optional<StringData> metaField,
    NewFieldNames* newFieldNamesToBeInserted,
    int32_t* sizeToBeAdded) const {
    // BSON size for an object with an empty object field where field name is empty string.
    // We can use this as an offset to know the size when we have real field names.
    static constexpr int emptyObjSize = 12;
    // Validate in debug builds that this size is correct
    dassert(emptyObjSize == BSON("" << BSONObj()).objsize());

    newFieldNamesToBeInserted->clear();
    *sizeToBeAdded = 0;
    auto numMeasurementsFieldLength = numDigits(_numMeasurements);
    for (const auto& elem : doc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == metaField) {
            // Ignore the metadata field since it will not be inserted.
            continue;
        }

        auto hashedKey = StringSet::hasher().hashed_key(fieldName);
        if (!_fieldNames.contains(hashedKey)) {
            // Record the new field name only if it hasn't been committed yet. There could
            // be concurrent batches writing to this bucket with the same new field name,
            // but they're not guaranteed to commit successfully.
            newFieldNamesToBeInserted->push_back(hashedKey);

            // Only update the bucket size once to account for the new field name if it
            // isn't already pending a commit from another batch.
            if (!_uncommittedFieldNames.contains(hashedKey)) {
                // Add the size of an empty object with that field name.
                *sizeToBeAdded += emptyObjSize + fieldName.size();

                // The control.min and control.max summaries don't have any information for
                // this new field name yet. Add two measurements worth of data to account
                // for this. As this is the first measurement for this field, min == max.
                *sizeToBeAdded += elem.size() * 2;
            }
        }

        // Add the element size, taking into account that the name will be changed to its
        // positional number. Add 1 to the calculation since the element's field name size
        // accounts for a null terminator whereas the stringified position does not.
        *sizeToBeAdded += elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;
    }
}

bool BucketCatalog::Bucket::_hasBeenCommitted() const {
    return _numCommittedMeasurements != 0 || _preparedBatch;
}

std::shared_ptr<BucketCatalog::WriteBatch> BucketCatalog::Bucket::_activeBatch(
    OperationId opId, ExecutionStatsController& stats) {
    auto it = _batches.find(opId);
    if (it == _batches.end()) {
        it = _batches
                 .try_emplace(
                     opId,
                     std::make_shared<WriteBatch>(BucketHandle{_bucketId, _stripe}, opId, stats))
                 .first;
    }
    return it->second;
}

BucketCatalog::ClosedBucket::~ClosedBucket() {
    if (_bucketStateManager) {
        _bucketStateManager->changeBucketState(
            bucketId,
            [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
                return boost::none;
            });
    }
}

BucketCatalog::ClosedBucket::ClosedBucket(BucketStateManager* bsm,
                                          const BucketId& bucketId,
                                          const std::string& tf,
                                          boost::optional<uint32_t> nm,
                                          bool efr)
    : bucketId{bucketId},
      timeField{tf},
      numMeasurements{nm},
      eligibleForReopening{efr},
      _bucketStateManager{bsm} {
    invariant(_bucketStateManager);
    _bucketStateManager->changeBucketState(
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(input.has_value());
            return input.value().setFlag(BucketStateFlag::kPendingCompression);
        });
}

BucketCatalog::ClosedBucket::ClosedBucket(ClosedBucket&& other)
    : bucketId{std::move(other.bucketId)},
      timeField{std::move(other.timeField)},
      numMeasurements{other.numMeasurements},
      eligibleForReopening{other.eligibleForReopening},
      _bucketStateManager{other._bucketStateManager} {
    other._bucketStateManager = nullptr;
}

BucketCatalog::ClosedBucket& BucketCatalog::ClosedBucket::operator=(ClosedBucket&& other) {
    if (this != &other) {
        bucketId = std::move(other.bucketId);
        timeField = std::move(other.timeField);
        numMeasurements = other.numMeasurements;
        eligibleForReopening = other.eligibleForReopening;
        _bucketStateManager = other._bucketStateManager;
        other._bucketStateManager = nullptr;
    }
    return *this;
}

BucketCatalog::WriteBatch::WriteBatch(const BucketHandle& bucket,
                                      OperationId opId,
                                      ExecutionStatsController& stats)
    : _bucket{bucket}, _opId(opId), _stats(stats) {}

bool BucketCatalog::WriteBatch::claimCommitRights() {
    return !_commitRights.swap(true);
}

StatusWith<BucketCatalog::CommitInfo> BucketCatalog::WriteBatch::getResult() {
    if (!_promise.getFuture().isReady()) {
        _stats.incNumWaits();
    }
    return _promise.getFuture().getNoThrow();
}

const BucketCatalog::BucketHandle& BucketCatalog::WriteBatch::bucket() const {
    return _bucket;
}

const BucketCatalog::BatchMeasurements& BucketCatalog::WriteBatch::measurements() const {
    return _measurements;
}

const BSONObj& BucketCatalog::WriteBatch::min() const {
    return _min;
}

const BSONObj& BucketCatalog::WriteBatch::max() const {
    return _max;
}

const StringMap<std::size_t>& BucketCatalog::WriteBatch::newFieldNamesToBeInserted() const {
    return _newFieldNamesToBeInserted;
}

uint32_t BucketCatalog::WriteBatch::numPreviouslyCommittedMeasurements() const {
    return _numPreviouslyCommittedMeasurements;
}

bool BucketCatalog::WriteBatch::needToDecompressBucketBeforeInserting() const {
    return _needToDecompressBucketBeforeInserting;
}

bool BucketCatalog::WriteBatch::finished() const {
    return _promise.getFuture().isReady();
}

BSONObj BucketCatalog::WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) { return nameHashPair.first; };
    return BSON("docs" << std::vector<BSONObj>(_measurements.begin(), _measurements.end())
                       << "bucketMin" << _min << "bucketMax" << _max << "numCommittedMeasurements"
                       << int(_numPreviouslyCommittedMeasurements) << "newFieldNamesToBeInserted"
                       << std::set<std::string>(
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.begin(),
                                                             toFieldName),
                              boost::make_transform_iterator(_newFieldNamesToBeInserted.end(),
                                                             toFieldName)));
}

void BucketCatalog::WriteBatch::_addMeasurement(const BSONObj& doc) {
    _measurements.push_back(doc);
}

void BucketCatalog::WriteBatch::_recordNewFields(Bucket* bucket, NewFieldNames&& fields) {
    for (auto&& field : fields) {
        _newFieldNamesToBeInserted[field] = field.hash();
        bucket->_uncommittedFieldNames.emplace(field);
    }
}

void BucketCatalog::WriteBatch::_prepareCommit(Bucket* bucket) {
    invariant(_commitRights.load());
    _numPreviouslyCommittedMeasurements = bucket->_numCommittedMeasurements;

    // Filter out field names that were new at the time of insertion, but have since been committed
    // by someone else.
    for (auto it = _newFieldNamesToBeInserted.begin(); it != _newFieldNamesToBeInserted.end();) {
        StringMapHashedKey fieldName(it->first, it->second);
        bucket->_uncommittedFieldNames.erase(fieldName);
        if (bucket->_fieldNames.contains(fieldName)) {
            _newFieldNamesToBeInserted.erase(it++);
            continue;
        }

        bucket->_fieldNames.emplace(fieldName);
        ++it;
    }

    for (const auto& doc : _measurements) {
        bucket->_minmax.update(
            doc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());
    }

    const bool isUpdate = _numPreviouslyCommittedMeasurements > 0;
    if (isUpdate) {
        _min = bucket->_minmax.minUpdates();
        _max = bucket->_minmax.maxUpdates();
    } else {
        _min = bucket->_minmax.min();
        _max = bucket->_minmax.max();

        // Approximate minmax memory usage by taking sizes of initial commit. Subsequent updates may
        // add fields but are most likely just to update values.
        bucket->_memoryUsage += _min.objsize();
        bucket->_memoryUsage += _max.objsize();
    }
}

void BucketCatalog::WriteBatch::_finish(const CommitInfo& info) {
    invariant(_commitRights.load());
    _promise.emplaceValue(info);
}

void BucketCatalog::WriteBatch::_abort(const Status& status) {
    if (finished()) {
        return;
    }

    _promise.setError(status);
}

BucketCatalog& BucketCatalog::get(ServiceContext* svcCtx) {
    return getBucketCatalog(svcCtx);
}

BucketCatalog& BucketCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

Status BucketCatalog::reopenBucket(OperationContext* opCtx,
                                   const CollectionPtr& coll,
                                   const BSONObj& bucketDoc) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();
    invariant(options,
              str::stream() << "Attempting to reopen a bucket for a non-timeseries collection: "
                            << ns);

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(*metaFieldName);
    }
    auto key = BucketKey{ns, BucketMetadata{metadata, coll->getDefaultCollator()}};

    // Validate the bucket document against the schema.
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return coll->checkValidation(opCtx, bucketDoc);
    };

    auto stats = _getExecutionStats(ns);

    auto res = _rehydrateBucket(opCtx,
                                ns,
                                coll->getDefaultCollator(),
                                *options,
                                BucketToReopen{bucketDoc, validator},
                                boost::none);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto bucket = std::move(res.getValue());

    auto stripeNumber = _getStripeNumber(key);

    // Register the reopened bucket with the catalog.
    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    ClosedBuckets closedBuckets;
    return _reopenBucket(&stripe,
                         stripeLock,
                         stats,
                         key,
                         std::move(bucket),
                         _bucketStateManager.getEra(),
                         &closedBuckets)
        .getStatus();
}

BSONObj BucketCatalog::getMetadata(const BucketHandle& handle) {
    auto const& stripe = _stripes[handle.stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    const Bucket* bucket = _findBucket(stripe, stripeLock, handle.bucketId);
    if (!bucket) {
        return {};
    }

    return bucket->_metadata.toBSON();
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::tryInsert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine) {
    return _insert(opCtx, ns, comparator, options, doc, combine, AllowBucketCreation::kNo);
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    BucketFindResult bucketFindResult) {
    return _insert(
        opCtx, ns, comparator, options, doc, combine, AllowBucketCreation::kYes, bucketFindResult);
}

Status BucketCatalog::prepareCommit(std::shared_ptr<WriteBatch> batch) {
    auto getBatchStatus = [&] { return batch->_promise.getFuture().getNoThrow().getStatus(); };

    if (batch->finished()) {
        // In this case, someone else aborted the batch behind our back. Oops.
        return getBatchStatus();
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    _waitToCommitBatch(&stripe, batch);

    stdx::lock_guard stripeLock{stripe.mutex};
    Bucket* bucket = _useBucketAndChangeState(
        &stripe,
        stripeLock,
        batch->bucket().bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(input.has_value());
            return input.value().setFlag(BucketStateFlag::kPrepared);
        });

    if (batch->finished()) {
        // Someone may have aborted it while we were waiting. Since we have the prepared batch, we
        // should now be able to fully abort the bucket.
        if (bucket) {
            _abort(&stripe, stripeLock, batch, getBatchStatus());
        }
        return getBatchStatus();
    } else if (!bucket) {
        _abort(&stripe,
               stripeLock,
               batch,
               getTimeseriesBucketClearedError(batch->bucket().bucketId.ns,
                                               batch->bucket().bucketId.oid));
        return getBatchStatus();
    }

    auto prevMemoryUsage = bucket->_memoryUsage;
    batch->_prepareCommit(bucket);
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage - prevMemoryUsage);

    return Status::OK();
}

boost::optional<BucketCatalog::ClosedBucket> BucketCatalog::finish(
    std::shared_ptr<WriteBatch> batch, const CommitInfo& info) {
    invariant(!batch->finished());

    boost::optional<ClosedBucket> closedBucket;

    batch->_finish(info);

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    Bucket* bucket = _useBucketAndChangeState(
        &stripe,
        stripeLock,
        batch->bucket().bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(input.has_value());
            return input.value().unsetFlag(BucketStateFlag::kPrepared);
        });
    if (bucket) {
        bucket->_preparedBatch.reset();
    }

    auto& stats = batch->_stats;
    stats.incNumCommits();
    if (batch->numPreviouslyCommittedMeasurements() == 0) {
        stats.incNumBucketInserts();
    } else {
        stats.incNumBucketUpdates();
    }

    stats.incNumMeasurementsCommitted(batch->measurements().size());
    if (bucket) {
        bucket->_numCommittedMeasurements += batch->measurements().size();
    }

    if (!bucket) {
        // It's possible that we cleared the bucket in between preparing the commit and finishing
        // here. In this case, we should abort any other ongoing batches and clear the bucket from
        // the catalog so it's not hanging around idle.
        auto it = stripe.allBuckets.find(batch->bucket().bucketId);
        if (it != stripe.allBuckets.end()) {
            bucket = it->second.get();
            bucket->_preparedBatch.reset();
            _abort(&stripe,
                   stripeLock,
                   bucket,
                   nullptr,
                   getTimeseriesBucketClearedError(bucket->ns(), bucket->oid()));
        }
    } else if (bucket->allCommitted()) {
        switch (bucket->_rolloverAction) {
            case RolloverAction::kHardClose:
            case RolloverAction::kSoftClose: {
                const bool eligibleForReopening =
                    bucket->_rolloverAction == RolloverAction::kSoftClose;
                closedBucket = boost::in_place(&_bucketStateManager,
                                               bucket->bucketId(),
                                               bucket->getTimeField().toString(),
                                               bucket->numMeasurements(),
                                               eligibleForReopening);
                _removeBucket(&stripe, stripeLock, bucket, RemovalMode::kClose);
                break;
            }
            case RolloverAction::kArchive: {
                ClosedBuckets closedBuckets;
                _archiveBucket(&stripe, stripeLock, bucket, &closedBuckets);
                if (!closedBuckets.empty()) {
                    closedBucket = std::move(closedBuckets[0]);
                }
                break;
            }
            case RolloverAction::kNone: {
                _markBucketIdle(&stripe, stripeLock, bucket);
                break;
            }
        }
    }
    return closedBucket;
}

void BucketCatalog::abort(std::shared_ptr<WriteBatch> batch, const Status& status) {
    invariant(batch);
    invariant(batch->_commitRights.load());

    if (batch->finished()) {
        return;
    }

    auto& stripe = _stripes[batch->bucket().stripe];
    stdx::lock_guard stripeLock{stripe.mutex};

    _abort(&stripe, stripeLock, batch, status);
}

void BucketCatalog::directWriteStart(const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    auto result = _bucketStateManager.changeBucketState(
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (input.has_value()) {
                return input.value().setFlag(BucketStateFlag::kPendingDirectWrite);
            }
            // The underlying bucket isn't tracked by the catalog, but we need to insert a state
            // here so that we can conflict reopening this bucket until we've completed our write
            // and the reader has refetched.
            return BucketState{}
                .setFlag(BucketStateFlag::kPendingDirectWrite)
                .setFlag(BucketStateFlag::kUntracked);
        });
    if (result && result.value().isPrepared()) {
        hangTimeseriesDirectModificationBeforeWriteConflict.pauseWhileSet();
        throwWriteConflictException("Prepared bucket can no longer be inserted into.");
    }
    hangTimeseriesDirectModificationAfterStart.pauseWhileSet();
}

void BucketCatalog::directWriteFinish(const NamespaceString& ns, const OID& oid) {
    invariant(!ns.isTimeseriesBucketsCollection());
    hangTimeseriesDirectModificationBeforeFinish.pauseWhileSet();
    (void)_bucketStateManager.changeBucketState(
        BucketId{ns, oid},
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                // We may have had multiple direct writes to this document in the same storage
                // transaction. If so, a previous call to directWriteFinish may have cleaned up the
                // state.
                return boost::none;
            }
            if (input.value().isSet(BucketStateFlag::kUntracked)) {
                // The underlying bucket is not tracked by the catalog, so we can clean up the
                // state.
                return boost::none;
            }
            return input.value()
                .unsetFlag(BucketStateFlag::kPendingDirectWrite)
                .setFlag(BucketStateFlag::kCleared);
        });
}

void BucketCatalog::clear(ShouldClearFn&& shouldClear) {
    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        _bucketStateManager.clearSetOfBuckets(std::move(shouldClear));
        return;
    }
    for (auto& stripe : _stripes) {
        stdx::lock_guard stripeLock{stripe.mutex};
        for (auto it = stripe.allBuckets.begin(); it != stripe.allBuckets.end();) {
            auto nextIt = std::next(it);

            const auto& bucket = it->second;
            if (shouldClear(bucket->ns())) {
                {
                    stdx::lock_guard catalogLock{_mutex};
                    _executionStats.erase(bucket->ns());
                }
                _abort(&stripe,
                       stripeLock,
                       bucket.get(),
                       nullptr,
                       getTimeseriesBucketClearedError(bucket->ns(), bucket->oid()));
            }

            it = nextIt;
        }
    }
}

void BucketCatalog::clear(const NamespaceString& ns) {
    invariant(!ns.isTimeseriesBucketsCollection());
    clear([ns](const NamespaceString& bucketNs) { return bucketNs == ns; });
}

void BucketCatalog::clear(StringData dbName) {
    clear([dbName = dbName.toString()](const NamespaceString& bucketNs) {
        return bucketNs.db() == dbName;
    });
}

void BucketCatalog::_appendExecutionStatsToBuilder(const ExecutionStats* stats,
                                                   BSONObjBuilder* builder) const {
    builder->appendNumber("numBucketInserts", stats->numBucketInserts.load());
    builder->appendNumber("numBucketUpdates", stats->numBucketUpdates.load());
    builder->appendNumber("numBucketsOpenedDueToMetadata",
                          stats->numBucketsOpenedDueToMetadata.load());
    builder->appendNumber("numBucketsClosedDueToCount", stats->numBucketsClosedDueToCount.load());
    builder->appendNumber("numBucketsClosedDueToSchemaChange",
                          stats->numBucketsClosedDueToSchemaChange.load());
    builder->appendNumber("numBucketsClosedDueToSize", stats->numBucketsClosedDueToSize.load());
    builder->appendNumber("numBucketsClosedDueToTimeForward",
                          stats->numBucketsClosedDueToTimeForward.load());
    builder->appendNumber("numBucketsClosedDueToTimeBackward",
                          stats->numBucketsClosedDueToTimeBackward.load());
    builder->appendNumber("numBucketsClosedDueToMemoryThreshold",
                          stats->numBucketsClosedDueToMemoryThreshold.load());

    auto commits = stats->numCommits.load();
    builder->appendNumber("numCommits", commits);
    builder->appendNumber("numWaits", stats->numWaits.load());
    auto measurementsCommitted = stats->numMeasurementsCommitted.load();
    builder->appendNumber("numMeasurementsCommitted", measurementsCommitted);
    if (commits) {
        builder->appendNumber("avgNumMeasurementsPerCommit", measurementsCommitted / commits);
    }

    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        builder->appendNumber("numBucketsClosedDueToReopening",
                              stats->numBucketsClosedDueToReopening.load());
        builder->appendNumber("numBucketsArchivedDueToMemoryThreshold",
                              stats->numBucketsArchivedDueToMemoryThreshold.load());
        builder->appendNumber("numBucketsArchivedDueToTimeBackward",
                              stats->numBucketsArchivedDueToTimeBackward.load());
        builder->appendNumber("numBucketsReopened", stats->numBucketsReopened.load());
        builder->appendNumber("numBucketsKeptOpenDueToLargeMeasurements",
                              stats->numBucketsKeptOpenDueToLargeMeasurements.load());
        builder->appendNumber("numBucketsClosedDueToCachePressure",
                              stats->numBucketsClosedDueToCachePressure.load());
        builder->appendNumber("numBucketsFetched", stats->numBucketsFetched.load());
        builder->appendNumber("numBucketsQueried", stats->numBucketsQueried.load());
        builder->appendNumber("numBucketFetchesFailed", stats->numBucketFetchesFailed.load());
        builder->appendNumber("numBucketQueriesFailed", stats->numBucketQueriesFailed.load());
        builder->appendNumber("numBucketReopeningsFailed", stats->numBucketReopeningsFailed.load());
        builder->appendNumber("numDuplicateBucketsReopened",
                              stats->numDuplicateBucketsReopened.load());
    }
}

void BucketCatalog::appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const {
    invariant(!ns.isTimeseriesBucketsCollection());
    const std::shared_ptr<ExecutionStats> stats = _getExecutionStats(ns);
    _appendExecutionStatsToBuilder(stats.get(), builder);
}

void BucketCatalog::appendGlobalExecutionStats(BSONObjBuilder* builder) const {
    _appendExecutionStatsToBuilder(&_globalExecutionStats, builder);
}

void BucketCatalog::appendStateManagementStats(BSONObjBuilder* builder) const {
    _bucketStateManager.appendStats(builder);
}

long long BucketCatalog::memoryUsage() const {
    return _memoryUsage.load();
}

BucketCatalog::BucketMetadata::BucketMetadata(BSONElement elem,
                                              const StringData::ComparatorInterface* comparator)
    : _metadataElement(elem), _comparator(comparator) {
    if (_metadataElement) {
        BSONObjBuilder objBuilder;
        // We will get an object of equal size, just with reordered fields.
        objBuilder.bb().reserveBytes(_metadataElement.size());
        timeseries::normalizeMetadata(&objBuilder, _metadataElement, boost::none);
        _metadata = objBuilder.obj();
    }
    // Updates the BSONElement to refer to the copied BSONObj.
    _metadataElement = _metadata.firstElement();
}

bool BucketCatalog::BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

const BSONObj& BucketCatalog::BucketMetadata::toBSON() const {
    return _metadata;
}

const BSONElement& BucketCatalog::BucketMetadata::element() const {
    return _metadataElement;
}

StringData BucketCatalog::BucketMetadata::getMetaField() const {
    return StringData(_metadataElement.fieldName());
}

const StringData::ComparatorInterface* BucketCatalog::BucketMetadata::getComparator() const {
    return _comparator;
}

BucketCatalog::BucketKey::BucketKey(const NamespaceString& n, const BucketMetadata& m)
    : ns(n), metadata(m), hash(absl::Hash<BucketKey>{}(*this)) {}

BucketCatalog::BucketId::BucketId(const NamespaceString& n, const OID& o)
    : ns{n}, oid{o}, hash{absl::Hash<BucketId>{}(*this)} {}

std::size_t BucketCatalog::BucketHasher::operator()(const BucketKey& key) const {
    // Use the default absl hasher.
    return key.hash;
}

std::size_t BucketCatalog::BucketHasher::operator()(const BucketId& bucketId) const {
    // Use the default absl hasher.
    return bucketId.hash;
}

std::size_t BucketCatalog::BucketHasher::operator()(const BucketKey::Hash& key) const {
    return key;
}

BucketCatalog::ArchivedBucket::ArchivedBucket(const BucketId& b, const std::string& t)
    : bucketId{b}, timeField{t} {}

StatusWith<std::pair<BucketCatalog::BucketKey, Date_t>> BucketCatalog::_extractBucketingParameters(
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc) const {
    Date_t time;
    BSONElement metadata;

    if (!options.getMetaField().has_value()) {
        auto swTime = timeseries::extractTime(doc, options.getTimeField());
        if (!swTime.isOK()) {
            return swTime.getStatus();
        }
        time = swTime.getValue();
    } else {
        auto swDocTimeAndMeta = timeseries::extractTimeAndMeta(
            doc, options.getTimeField(), options.getMetaField().value());
        if (!swDocTimeAndMeta.isOK()) {
            return swDocTimeAndMeta.getStatus();
        }
        time = swDocTimeAndMeta.getValue().first;
        metadata = swDocTimeAndMeta.getValue().second;
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};

    return {std::make_pair(key, time)};
}

BucketCatalog::StripeNumber BucketCatalog::_getStripeNumber(const BucketKey& key) const {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % kNumberOfStripes;
}

const BucketCatalog::Bucket* BucketCatalog::_findBucket(const Stripe& stripe,
                                                        WithLock,
                                                        const BucketId& bucketId,
                                                        IgnoreBucketState mode) {
    auto it = stripe.allBuckets.find(bucketId);
    if (it != stripe.allBuckets.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = _bucketStateManager.getBucketState(it->second.get());
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                                 WithLock stripeLock,
                                                 const BucketId& bucketId,
                                                 IgnoreBucketState mode) {
    return const_cast<Bucket*>(_findBucket(*stripe, stripeLock, bucketId, mode));
}

BucketCatalog::Bucket* BucketCatalog::_useBucketAndChangeState(
    Stripe* stripe,
    WithLock stripeLock,
    const BucketId& bucketId,
    const BucketStateManager::StateChangeFn& change) {
    auto it = stripe->allBuckets.find(bucketId);
    if (it != stripe->allBuckets.end()) {
        if (auto state = _bucketStateManager.changeBucketState(it->second.get(), change);
            state && !state.value().conflictsWithInsertion()) {
            return it->second.get();
        }
    }
    return nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useBucket(Stripe* stripe,
                                                 WithLock stripeLock,
                                                 const CreationInfo& info,
                                                 AllowBucketCreation mode) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
        // No open bucket for this metadata.
        return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info)
                                                 : nullptr;
    }

    auto& openSet = it->second;
    Bucket* bucket = nullptr;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->_rolloverAction == RolloverAction::kNone) {
            bucket = potentialBucket;
            break;
        }
    }
    if (!bucket) {
        return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info)
                                                 : nullptr;
    }

    if (auto state = _bucketStateManager.getBucketState(bucket);
        state && !state.value().conflictsWithInsertion()) {
        _markBucketNotIdle(stripe, stripeLock, bucket);
        return bucket;
    }

    _abort(stripe,
           stripeLock,
           bucket,
           nullptr,
           getTimeseriesBucketClearedError(bucket->ns(), bucket->oid()));

    return mode == AllowBucketCreation::kYes ? _allocateBucket(stripe, stripeLock, info) : nullptr;
}

BucketCatalog::Bucket* BucketCatalog::_useAlternateBucket(Stripe* stripe,
                                                          WithLock stripeLock,
                                                          const CreationInfo& info) {
    auto it = stripe->openBuckets.find(info.key);
    if (it == stripe->openBuckets.end()) {
        // No open bucket for this metadata.
        return nullptr;
    }

    auto& openSet = it->second;
    // In order to potentially erase elements of the set while we iterate it (via _abort), we need
    // to advance the iterator before we call erase. This means we can't use the more
    // straightforward range iteration, and use the somewhat awkward pattern below.
    for (auto it = openSet.begin(); it != openSet.end();) {
        Bucket* potentialBucket = *it++;

        if (potentialBucket->_rolloverAction == RolloverAction::kNone ||
            potentialBucket->_rolloverAction == RolloverAction::kHardClose) {
            continue;
        }

        auto bucketTime = potentialBucket->getTime();
        if (info.time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds()) ||
            info.time < bucketTime) {
            continue;
        }

        auto state = _bucketStateManager.getBucketState(potentialBucket);
        invariant(state);
        if (!state.value().conflictsWithInsertion()) {
            invariant(!potentialBucket->_idleListEntry.has_value());
            return potentialBucket;
        }

        // If we still have an entry for the bucket in the open set, but it conflicts with
        // insertion, then it must have been cleared, and we can clean it up.
        invariant(state.value().isSet(BucketStateFlag::kCleared));
        _abort(stripe,
               stripeLock,
               potentialBucket,
               nullptr,
               getTimeseriesBucketClearedError(potentialBucket->bucketId().ns,
                                               potentialBucket->bucketId().oid));
    }

    return nullptr;
}

StatusWith<std::unique_ptr<BucketCatalog::Bucket>> BucketCatalog::_rehydrateBucket(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BucketToReopen& bucketToReopen,
    boost::optional<const BucketKey&> expectedKey) {
    invariant(feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility));
    const auto& [bucketDoc, validator, catalogEra] = bucketToReopen;
    if (catalogEra < _bucketStateManager.getEra()) {
        return {ErrorCodes::WriteConflict, "Bucket is from an earlier era, may be outdated"};
    }

    BSONElement bucketIdElem = bucketDoc.getField(timeseries::kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::jstOID) {
        return {ErrorCodes::BadValue,
                str::stream() << timeseries::kBucketIdFieldName
                              << " is missing or not an ObjectId"};
    }

    // Validate the bucket document against the schema.
    auto result = validator(opCtx, bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        return result.second;
    }

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(timeseries::kBucketMetaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{ns, BucketMetadata{metadata, comparator}};
    if (expectedKey.has_value() && key != expectedKey.value()) {
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }
    auto stripeNumber = _getStripeNumber(key);

    BucketId bucketId{key.ns, bucketIdElem.OID()};
    std::unique_ptr<Bucket> bucket =
        std::make_unique<Bucket>(bucketId, stripeNumber, key.hash, &_bucketStateManager);

    const bool isCompressed = timeseries::isCompressedBucket(bucketDoc);
    if (isCompressed) {
        // TODO (SERVER-69907): Allow opening compressed bucket
        return Status{ErrorCodes::BadValue, "Reopening uncompressed buckets is not supported yet"};
    }

    // Initialize the remaining member variables from the bucket document.
    bucket->_metadata = key.metadata;
    bucket->_timeField = options.getTimeField().toString();
    if (isCompressed) {
        auto decompressed = timeseries::decompressBucket(bucketDoc);
        if (!decompressed.has_value()) {
            return Status{ErrorCodes::BadValue, "Bucket could not be decompressed"};
        }
        bucket->_size = decompressed.value().objsize();
    } else {
        bucket->_size = bucketDoc.objsize();
    }
    auto controlField = bucketDoc.getObjectField(timeseries::kBucketControlFieldName);
    bucket->_minTime = controlField.getObjectField(timeseries::kBucketControlMinFieldName)
                           .getField(options.getTimeField())
                           .Date();

    // Populate the top-level data field names.
    const BSONObj& dataObj = bucketDoc.getObjectField(timeseries::kBucketDataFieldName);
    for (const BSONElement& dataElem : dataObj) {
        auto hashedKey = StringSet::hasher().hashed_key(dataElem.fieldName());
        bucket->_fieldNames.emplace(hashedKey);
    }

    auto swMinMax = timeseries::generateMinMaxFromBucketDoc(bucketDoc, comparator);
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->_minmax = std::move(swMinMax.getValue());

    auto swSchema = timeseries::generateSchemaFromBucketDoc(bucketDoc, comparator);
    if (!swSchema.isOK()) {
        return swSchema.getStatus();
    }
    bucket->_schema = std::move(swSchema.getValue());

    uint32_t numMeasurements = 0;
    const BSONElement timeColumnElem = dataObj.getField(options.getTimeField());

    if (isCompressed && timeColumnElem.type() == BSONType::BinData) {
        BSONColumn storage{timeColumnElem};
        numMeasurements = storage.size();
    } else if (timeColumnElem.isABSONObj()) {
        numMeasurements = timeColumnElem.Obj().nFields();
    } else {
        return {ErrorCodes::BadValue,
                "Bucket data field is malformed (missing a valid time column)"};
    }

    bucket->_numMeasurements = numMeasurements;
    bucket->_numCommittedMeasurements = numMeasurements;

    // The namespace is stored two times: the bucket itself and openBuckets. We don't have a great
    // approximation for the _schema or _minmax data structure size, so we use the control field
    // size as an approximation for _minmax, and half that size for _schema. Since the metadata
    // is stored in the bucket, we need to add that as well. A unique pointer to the bucket is
    // stored once: allBuckets. A raw pointer to the bucket is stored at most twice: openBuckets,
    // idleBuckets.
    bucket->_memoryUsage += (key.ns.size() * 2) + 1.5 * controlField.objsize() +
        key.metadata.toBSON().objsize() + sizeof(Bucket) + sizeof(std::unique_ptr<Bucket>) +
        (sizeof(Bucket*) * 2);

    return {std::move(bucket)};
}

StatusWith<BucketCatalog::Bucket*> BucketCatalog::_reopenBucket(Stripe* stripe,
                                                                WithLock stripeLock,
                                                                ExecutionStatsController stats,
                                                                const BucketKey& key,
                                                                std::unique_ptr<Bucket>&& bucket,
                                                                std::uint64_t targetEra,
                                                                ClosedBuckets* closedBuckets) {
    invariant(bucket);

    _expireIdleBuckets(stripe, stripeLock, stats, closedBuckets);

    // We may need to initialize the bucket's state.
    bool conflicts = false;
    auto initializeStateFn =
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
        if (targetEra < currentEra ||
            (input.has_value() && input.value().conflictsWithReopening())) {
            conflicts = true;
            return input;
        }
        conflicts = false;
        return input.has_value() ? input.value() : BucketState{};
    };

    auto state = _bucketStateManager.changeBucketState(bucket->bucketId(), initializeStateFn);
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    if (auto setIt = stripe->archivedBuckets.find(key.hash);
        setIt != stripe->archivedBuckets.end()) {
        auto& archivedSet = setIt->second;
        if (auto bucketIt = archivedSet.find(bucket->getTime());
            bucketIt != archivedSet.end() && bucket->bucketId() == bucketIt->second.bucketId) {
            long long memory =
                _marginalMemoryUsageForArchivedBucket(bucketIt->second, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe->archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(bucketIt);
            }
            _memoryUsage.fetchAndSubtract(memory);
            _numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    // Pass ownership of the reopened bucket to the bucket catalog.
    auto [insertedIt, newlyInserted] =
        stripe->allBuckets.try_emplace(bucket->bucketId(), std::move(bucket));
    invariant(newlyInserted);
    Bucket* unownedBucket = insertedIt->second.get();

    // If we already have an open bucket for this key, we need to close it.
    if (auto it = stripe->openBuckets.find(key); it != stripe->openBuckets.end()) {
        auto& openSet = it->second;
        for (Bucket* existingBucket : openSet) {
            if (existingBucket->_rolloverAction == RolloverAction::kNone) {
                stats.incNumBucketsClosedDueToReopening();
                if (existingBucket->allCommitted()) {
                    constexpr bool eligibleForReopening = true;
                    closedBuckets->emplace_back(
                        ClosedBucket{&_bucketStateManager,
                                     existingBucket->bucketId(),
                                     existingBucket->getTimeField().toString(),
                                     existingBucket->numMeasurements(),
                                     eligibleForReopening});
                    _removeBucket(stripe, stripeLock, existingBucket, RemovalMode::kClose);
                } else {
                    existingBucket->setRolloverAction(RolloverAction::kSoftClose);
                }
                // We should only have one open bucket at a time.
                break;
            }
        }
    }

    // Now actually mark this bucket as open.
    stripe->openBuckets[key].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    _memoryUsage.addAndFetch(unownedBucket->_memoryUsage);
    _numberOfActiveBuckets.fetchAndAdd(1);

    return unownedBucket;
}

StatusWith<BucketCatalog::Bucket*> BucketCatalog::_reuseExistingBucket(
    Stripe* stripe,
    WithLock stripeLock,
    ExecutionStatsController* stats,
    const BucketKey& key,
    Bucket* existingBucket,
    std::uint64_t targetEra) {
    invariant(existingBucket);

    // If we have an existing bucket, passing the Bucket* will let us check if the bucket was
    // cleared as part of a set since the last time it was used. If we were to just check by
    // OID, we may miss if e.g. there was a move chunk operation.
    bool conflicts = false;
    auto state = _bucketStateManager.changeBucketState(
        existingBucket,
        [targetEra, &conflicts](boost::optional<BucketState> input,
                                std::uint64_t currentEra) -> boost::optional<BucketState> {
            if (targetEra < currentEra ||
                (input.has_value() && input.value().conflictsWithReopening())) {
                conflicts = true;
                return input;
            }
            conflicts = false;
            return input.has_value() ? input.value() : BucketState{};
        });
    if (state.has_value() && state.value().isSet(BucketStateFlag::kCleared)) {
        _removeBucket(stripe, stripeLock, existingBucket, RemovalMode::kAbort);
        conflicts = true;
    }
    if (conflicts) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // It's possible to have two buckets with the same ID in different collections, so let's make
    // extra sure the existing bucket is the right one.
    if (existingBucket->ns() != key.ns) {
        return {ErrorCodes::BadValue, "Cannot re-use bucket: same ID but different namespace"};
    }

    // If the bucket was already open, wasn't cleared, the state didn't conflict with reopening, and
    // the namespace matches, then we can simply return it.
    stats->incNumDuplicateBucketsReopened();
    _markBucketNotIdle(stripe, stripeLock, existingBucket);

    return existingBucket;
}

StatusWith<BucketCatalog::InsertResult> BucketCatalog::_insert(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const StringData::ComparatorInterface* comparator,
    const TimeseriesOptions& options,
    const BSONObj& doc,
    CombineWithInsertsFromOtherClients combine,
    AllowBucketCreation mode,
    BucketFindResult bucketFindResult) {
    invariant(!ns.isTimeseriesBucketsCollection());

    auto res = _extractBucketingParameters(ns, comparator, options, doc);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto& key = res.getValue().first;
    auto time = res.getValue().second;

    ExecutionStatsController stats = _getExecutionStats(ns);
    _updateBucketFetchAndQueryStats(stats, bucketFindResult);

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto stripeNumber = _getStripeNumber(key);

    InsertResult result;
    result.catalogEra = _bucketStateManager.getEra();
    CreationInfo info{key, stripeNumber, time, options, stats, &result.closedBuckets};
    boost::optional<BucketToReopen> bucketToReopen = std::move(bucketFindResult.bucketToReopen);

    auto rehydratedBucket = bucketToReopen.has_value()
        ? _rehydrateBucket(opCtx, ns, comparator, options, bucketToReopen.value(), key)
        : StatusWith<std::unique_ptr<Bucket>>{ErrorCodes::BadValue, "No bucket to rehydrate"};
    if (rehydratedBucket.getStatus().code() == ErrorCodes::WriteConflict) {
        stats.incNumBucketReopeningsFailed();
        return rehydratedBucket.getStatus();
    }

    auto& stripe = _stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    if (rehydratedBucket.isOK()) {
        invariant(mode == AllowBucketCreation::kYes);
        hangTimeseriesInsertBeforeReopeningBucket.pauseWhileSet();

        StatusWith<Bucket*> swBucket{nullptr};
        auto existingIt = stripe.allBuckets.find(rehydratedBucket.getValue()->bucketId());
        if (existingIt != stripe.allBuckets.end()) {
            // First let's check the existing bucket if we have one.
            Bucket* existingBucket = existingIt->second.get();
            swBucket = _reuseExistingBucket(
                &stripe, stripeLock, &stats, key, existingBucket, bucketToReopen->catalogEra);
        } else {
            // No existing bucket to use, go ahead and try to reopen our rehydrated bucket.
            swBucket = _reopenBucket(&stripe,
                                     stripeLock,
                                     stats,
                                     key,
                                     std::move(rehydratedBucket.getValue()),
                                     bucketToReopen->catalogEra,
                                     &result.closedBuckets);
        }

        if (swBucket.isOK()) {
            Bucket* bucket = swBucket.getValue();
            invariant(bucket);
            auto insertionResult = _insertIntoBucket(opCtx,
                                                     &stripe,
                                                     stripeLock,
                                                     doc,
                                                     combine,
                                                     mode,
                                                     &info,
                                                     bucket,
                                                     &result.closedBuckets);
            auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
            invariant(batch);
            result.batch = *batch;

            return std::move(result);
        } else {
            stats.incNumBucketReopeningsFailed();
            if (swBucket.getStatus().code() == ErrorCodes::WriteConflict) {
                return swBucket.getStatus();
            }
            // If we had a different type of error, then we should fall through and proceed to open
            // a new bucket.
        }
    }

    Bucket* bucket = _useBucket(&stripe, stripeLock, info, mode);
    if (!bucket) {
        invariant(mode == AllowBucketCreation::kNo);
        constexpr bool allowQueryBasedReopening = true;
        result.candidate =
            _getReopeningCandidate(&stripe, stripeLock, info, allowQueryBasedReopening);
        return std::move(result);
    }

    auto insertionResult = _insertIntoBucket(
        opCtx, &stripe, stripeLock, doc, combine, mode, &info, bucket, &result.closedBuckets);
    if (auto* reason = stdx::get_if<RolloverReason>(&insertionResult)) {
        invariant(mode == AllowBucketCreation::kNo);
        if (bucket->allCommitted()) {
            _markBucketIdle(&stripe, stripeLock, bucket);
        }

        // If we were time forward or backward, we might be able to "reopen" a bucket we still have
        // in memory that's set to be closed when pending operations finish.
        if ((*reason == RolloverReason::kTimeBackward || *reason == RolloverReason::kTimeForward)) {
            if (Bucket* alternate = _useAlternateBucket(&stripe, stripeLock, info)) {
                insertionResult = _insertIntoBucket(opCtx,
                                                    &stripe,
                                                    stripeLock,
                                                    doc,
                                                    combine,
                                                    mode,
                                                    &info,
                                                    alternate,
                                                    &result.closedBuckets);
                if (auto* batch = stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
                    result.batch = *batch;
                    return std::move(result);
                }

                // We weren't able to insert into the other bucket, so fall through to the regular
                // reopening procedure.
            }
        }

        bool allowQueryBasedReopening = (*reason == RolloverReason::kTimeBackward);
        result.candidate =
            _getReopeningCandidate(&stripe, stripeLock, info, allowQueryBasedReopening);
    } else {
        result.batch = *stdx::get_if<std::shared_ptr<WriteBatch>>(&insertionResult);
    }
    return std::move(result);
}

stdx::variant<std::shared_ptr<BucketCatalog::WriteBatch>, BucketCatalog::RolloverReason>
BucketCatalog::_insertIntoBucket(OperationContext* opCtx,
                                 Stripe* stripe,
                                 WithLock stripeLock,
                                 const BSONObj& doc,
                                 CombineWithInsertsFromOtherClients combine,
                                 AllowBucketCreation mode,
                                 CreationInfo* info,
                                 Bucket* bucket,
                                 ClosedBuckets* closedBuckets) {
    NewFieldNames newFieldNamesToBeInserted;
    int32_t sizeToBeAdded = 0;

    bool isNewlyOpenedBucket = (bucket->_size == 0);
    if (!isNewlyOpenedBucket) {
        auto [action, reason] = _determineRolloverAction(
            opCtx, doc, info, bucket, &newFieldNamesToBeInserted, &sizeToBeAdded, mode);
        if ((action == RolloverAction::kSoftClose || action == RolloverAction::kArchive) &&
            mode == AllowBucketCreation::kNo) {
            // We don't actually want to roll this bucket over yet, bail out.
            return reason;
        } else if (action != RolloverAction::kNone) {
            info->openedDuetoMetadata = false;
            bucket = _rollover(stripe, stripeLock, bucket, *info, action);
            isNewlyOpenedBucket = true;
        }
    }
    if (isNewlyOpenedBucket) {
        bucket->_calculateBucketFieldsAndSizeChange(
            doc, info->options.getMetaField(), &newFieldNamesToBeInserted, &sizeToBeAdded);
    }

    auto batch = bucket->_activeBatch(getOpId(opCtx, combine), info->stats);
    batch->_addMeasurement(doc);
    batch->_recordNewFields(bucket, std::move(newFieldNamesToBeInserted));

    bucket->_numMeasurements++;
    bucket->_size += sizeToBeAdded;
    if (isNewlyOpenedBucket) {
        // The metadata only needs to be set if this bucket was newly created.
        bucket->_metadata = info->key.metadata;

        // The namespace is stored two times: the bucket itself and openBuckets.
        // We don't have a great approximation for the
        // _schema size, so we use initial document size minus metadata as an approximation. Since
        // the metadata itself is stored once, in the bucket, we can combine the two and just use
        // the initial document size. A unique pointer to the bucket is stored once: allBuckets. A
        // raw pointer to the bucket is stored at most twice: openBuckets, idleBuckets.
        bucket->_memoryUsage += (info->key.ns.size() * 2) + doc.objsize() + sizeof(Bucket) +
            sizeof(std::unique_ptr<Bucket>) + (sizeof(Bucket*) * 2);

        auto updateStatus = bucket->_schema.update(
            doc, info->options.getMetaField(), info->key.metadata.getComparator());
        invariant(updateStatus == timeseries::Schema::UpdateStatus::Updated);
    } else {
        _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    }
    _memoryUsage.fetchAndAdd(bucket->_memoryUsage);

    return batch;
}

void BucketCatalog::_waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        std::shared_ptr<WriteBatch> current;

        {
            stdx::lock_guard stripeLock{stripe->mutex};
            Bucket* bucket =
                _useBucket(stripe, stripeLock, batch->bucket().bucketId, IgnoreBucketState::kNo);
            if (!bucket || batch->finished()) {
                return;
            }

            current = bucket->_preparedBatch;
            if (!current) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->_preparedBatch = batch;
                bucket->_batches.erase(batch->_opId);
                return;
            }
        }

        // We have to wait for someone else to finish.
        current->getResult().getStatus().ignore();  // We don't care about the result.
    }
}

void BucketCatalog::_removeBucket(Stripe* stripe,
                                  WithLock stripeLock,
                                  Bucket* bucket,
                                  RemovalMode mode) {
    invariant(bucket->_batches.empty());
    invariant(!bucket->_preparedBatch);

    auto allIt = stripe->allBuckets.find(bucket->bucketId());
    invariant(allIt != stripe->allBuckets.end());

    _memoryUsage.fetchAndSubtract(bucket->_memoryUsage);
    _markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt = stripe->openBuckets.find({bucket->ns(), bucket->_metadata});
    if (openIt != stripe->openBuckets.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe->openBuckets.erase(openIt);
            } else {
                openSet.erase(bucketIt);
            }
        }
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    switch (mode) {
        case RemovalMode::kClose: {
            auto state = _bucketStateManager.getBucketState(bucket->bucketId());
            invariant(state.has_value());
            invariant(state.value().isSet(BucketStateFlag::kPendingCompression));
            break;
        }
        case RemovalMode::kAbort:
            _bucketStateManager.changeBucketState(
                bucket->bucketId(),
                [](boost::optional<BucketState> input,
                   std::uint64_t) -> boost::optional<BucketState> {
                    invariant(input.has_value());
                    if (input->conflictsWithReopening()) {
                        return input.value().setFlag(BucketStateFlag::kUntracked);
                    }
                    return boost::none;
                });
            break;
        case RemovalMode::kArchive:
            // No state change
            break;
    }

    _numberOfActiveBuckets.fetchAndSubtract(1);
    stripe->allBuckets.erase(allIt);
}

void BucketCatalog::_archiveBucket(Stripe* stripe,
                                   WithLock stripeLock,
                                   Bucket* bucket,
                                   ClosedBuckets* closedBuckets) {
    bool archived = false;
    auto& archivedSet = stripe->archivedBuckets[bucket->keyHash()];
    auto it = archivedSet.find(bucket->getTime());
    if (it == archivedSet.end()) {
        auto [it, inserted] = archivedSet.emplace(
            bucket->getTime(),
            ArchivedBucket{bucket->bucketId(), bucket->getTimeField().toString()});

        long long memory =
            _marginalMemoryUsageForArchivedBucket(it->second, archivedSet.size() == 1);
        _memoryUsage.fetchAndAdd(memory);
        archived = true;
    }

    RemovalMode mode = RemovalMode::kArchive;
    if (archived) {
        // If we have an archived bucket, we still want to account for it in numberOfActiveBuckets
        // so we will increase it here since removeBucket decrements the count.
        _numberOfActiveBuckets.fetchAndAdd(1);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        mode = RemovalMode::kClose;
        constexpr bool eligibleForReopening = true;
        closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                 bucket->bucketId(),
                                                 bucket->getTimeField().toString(),
                                                 bucket->numMeasurements(),
                                                 eligibleForReopening});
    }

    _removeBucket(stripe, stripeLock, bucket, mode);
}

boost::optional<OID> BucketCatalog::_findArchivedCandidate(Stripe* stripe,
                                                           WithLock stripeLock,
                                                           const CreationInfo& info) {
    auto setIt = stripe->archivedBuckets.find(info.key.hash);
    if (setIt == stripe->archivedBuckets.end()) {
        return boost::none;
    }

    auto& archivedSet = setIt->second;

    // We want to find the largest time that is not greater than info.time. Generally lower_bound
    // will return the smallest element not less than the search value, but we are using
    // std::greater instead of std::less for the map's comparisons. This means the order of keys
    // will be reversed, and lower_bound will return what we want.
    auto it = archivedSet.lower_bound(info.time);
    if (it == archivedSet.end()) {
        return boost::none;
    }

    const auto& [candidateTime, candidateBucket] = *it;
    invariant(candidateTime <= info.time);
    // We need to make sure our measurement can fit without violating max span. If not, we
    // can't use this bucket.
    if (info.time - candidateTime < Seconds(*info.options.getBucketMaxSpanSeconds())) {
        auto state = _bucketStateManager.getBucketState(candidateBucket.bucketId);
        if (state && !state.value().conflictsWithReopening()) {
            return candidateBucket.bucketId.oid;
        } else {
            if (state) {
                _bucketStateManager.changeBucketState(
                    candidateBucket.bucketId,
                    [](boost::optional<BucketState> input,
                       std::uint64_t) -> boost::optional<BucketState> {
                        if (!input.has_value()) {
                            return boost::none;
                        }
                        invariant(input.value().conflictsWithReopening());
                        return input.value().setFlag(BucketStateFlag::kUntracked);
                    });
            }
            long long memory =
                _marginalMemoryUsageForArchivedBucket(candidateBucket, archivedSet.size() == 1);
            if (archivedSet.size() == 1) {
                stripe->archivedBuckets.erase(setIt);
            } else {
                archivedSet.erase(it);
            }
            _memoryUsage.fetchAndSubtract(memory);
            _numberOfActiveBuckets.fetchAndSubtract(1);
        }
    }

    return boost::none;
}

stdx::variant<std::monostate, OID, BSONObj> BucketCatalog::_getReopeningCandidate(
    Stripe* stripe, WithLock stripeLock, const CreationInfo& info, bool allowQueryBasedReopening) {
    if (auto archived = _findArchivedCandidate(stripe, stripeLock, info)) {
        return archived.value();
    }

    if (!allowQueryBasedReopening) {
        return {};
    }

    boost::optional<BSONElement> metaElement;
    if (info.options.getMetaField().has_value()) {
        metaElement = info.key.metadata.element();
    }

    auto controlMinTimePath =
        timeseries::kControlMinFieldNamePrefix.toString() + info.options.getTimeField();

    return timeseries::generateReopeningFilters(
        info.time, metaElement, controlMinTimePath, *info.options.getBucketMaxSpanSeconds());
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket =
        _useBucket(stripe, stripeLock, batch->bucket().bucketId, IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        batch->_abort(status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    _abort(stripe, stripeLock, bucket, batch, status);
}

void BucketCatalog::_abort(Stripe* stripe,
                           WithLock stripeLock,
                           Bucket* bucket,
                           std::shared_ptr<WriteBatch> batch,
                           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket->_batches) {
        current->_abort(status);
    }
    bucket->_batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket->_preparedBatch) {
        if (prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            prepared->_abort(status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        _removeBucket(stripe, stripeLock, bucket, RemovalMode::kAbort);
    }
}

void BucketCatalog::_compressionDone(const BucketId& bucketId) {
    _bucketStateManager.changeBucketState(
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            return boost::none;
        });
}

void BucketCatalog::_markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    invariant(!bucket->_idleListEntry.has_value());
    invariant(bucket->allCommitted());
    stripe->idleBuckets.push_front(bucket);
    bucket->_idleListEntry = stripe->idleBuckets.begin();
}

void BucketCatalog::_markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket) {
    invariant(bucket);
    if (bucket->_idleListEntry.has_value()) {
        stripe->idleBuckets.erase(bucket->_idleListEntry.value());
        bucket->_idleListEntry = boost::none;
    }
}

void BucketCatalog::_expireIdleBuckets(Stripe* stripe,
                                       WithLock stripeLock,
                                       ExecutionStatsController& stats,
                                       std::vector<BucketCatalog::ClosedBucket>* closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
        serverGlobalParams.featureCompatibility);
    constexpr bool eligibleForReopening{true};

    while (!stripe->idleBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe->idleBuckets.back();

        auto state = _bucketStateManager.getBucketState(bucket);
        if (canArchive && state && !state.value().conflictsWithInsertion()) {
            // Can archive a bucket if it's still eligible for insertions.
            _archiveBucket(stripe, stripeLock, bucket, closedBuckets);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state && state.value().isSet(BucketStateFlag::kCleared)) {
            // Bucket was cleared and just needs to be removed from catalog.
            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kAbort);
        } else {
            closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                     bucket->bucketId(),
                                                     bucket->getTimeField().toString(),
                                                     bucket->numMeasurements(),
                                                     eligibleForReopening});
            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kClose);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (canArchive && !stripe->archivedBuckets.empty() &&
           _memoryUsage.load() > getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto& [hash, archivedSet] = *stripe->archivedBuckets.begin();
        invariant(!archivedSet.empty());

        auto& [timestamp, bucket] = *archivedSet.begin();
        closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                 bucket.bucketId,
                                                 bucket.timeField,
                                                 boost::none,
                                                 eligibleForReopening});

        long long memory = _marginalMemoryUsageForArchivedBucket(bucket, archivedSet.size() == 1);
        if (archivedSet.size() == 1) {
            // If this is the only entry, erase the whole map so we don't leave it empty.
            stripe->archivedBuckets.erase(stripe->archivedBuckets.begin());
        } else {
            // Otherwise just erase this bucket from the map.
            archivedSet.erase(archivedSet.begin());
        }
        _memoryUsage.fetchAndSubtract(memory);
        _numberOfActiveBuckets.fetchAndSubtract(1);

        stats.incNumBucketsClosedDueToMemoryThreshold();
        ++numExpired;
    }
}

BucketCatalog::Bucket* BucketCatalog::_allocateBucket(Stripe* stripe,
                                                      WithLock stripeLock,
                                                      const CreationInfo& info) {
    _expireIdleBuckets(stripe, stripeLock, info.stats, info.closedBuckets);

    auto [oid, roundedTime] = generateBucketOID(info.time, info.options);
    auto bucketId = BucketId{info.key.ns, oid};

    auto [it, inserted] = stripe->allBuckets.try_emplace(
        bucketId,
        std::make_unique<Bucket>(bucketId, info.stripe, info.key.hash, &_bucketStateManager));
    tassert(6130900, "Expected bucket to be inserted", inserted);
    Bucket* bucket = it->second.get();
    stripe->openBuckets[info.key].emplace(bucket);

    auto state = _bucketStateManager.changeBucketState(
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            invariant(!input.has_value());
            return BucketState{};
        });
    invariant(state == BucketState{});
    _numberOfActiveBuckets.fetchAndAdd(1);

    if (info.openedDuetoMetadata) {
        info.stats.incNumBucketsOpenedDueToMetadata();
    }

    bucket->_timeField = info.options.getTimeField().toString();
    bucket->_minTime = roundedTime;

    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->_minmax.update(
        controlDoc, bucket->_metadata.getMetaField(), bucket->_metadata.getComparator());
    return bucket;
}

std::pair<BucketCatalog::RolloverAction, BucketCatalog::RolloverReason>
BucketCatalog::_determineRolloverAction(OperationContext* opCtx,
                                        const BSONObj& doc,
                                        CreationInfo* info,
                                        Bucket* bucket,
                                        NewFieldNames* newFieldNamesToBeInserted,
                                        int32_t* sizeToBeAdded,
                                        AllowBucketCreation mode) {
    // If the mode is enabled to create new buckets, then we should update stats for soft closures
    // accordingly. If we specify the mode to not allow bucket creation, it means we are not sure if
    // we want to soft close the bucket yet and should wait to update closure stats.
    const bool shouldUpdateStats = (mode == AllowBucketCreation::kYes);

    auto bucketTime = bucket->getTime();
    if (info->time - bucketTime >= Seconds(*info->options.getBucketMaxSpanSeconds())) {
        if (shouldUpdateStats) {
            info->stats.incNumBucketsClosedDueToTimeForward();
        }
        return {RolloverAction::kSoftClose, RolloverReason::kTimeForward};
    }
    if (info->time < bucketTime) {
        const bool canArchive = feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility);
        if (shouldUpdateStats) {
            if (canArchive) {
                info->stats.incNumBucketsArchivedDueToTimeBackward();
            } else {
                info->stats.incNumBucketsClosedDueToTimeBackward();
            }
        }
        return {canArchive ? RolloverAction::kArchive : RolloverAction::kSoftClose,
                RolloverReason::kTimeBackward};
    }
    if (bucket->_numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        info->stats.incNumBucketsClosedDueToCount();
        return {RolloverAction::kHardClose, RolloverReason::kCount};
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure we
    // will decrease the size of buckets before we close them.
    int32_t cacheDerivedBucketMaxSize = getCacheDerivedBucketMaxSize(
        opCtx->getServiceContext()->getStorageEngine(), _numberOfActiveBuckets.load());
    int32_t effectiveMaxSize = std::min(gTimeseriesBucketMaxSize, cacheDerivedBucketMaxSize);

    // Before we hit our bucket minimum count, we will allow for large measurements to be inserted
    // into buckets. Instead of packing the bucket to the BSON size limit, 16MB, we'll limit the max
    // bucket size to 12MB. This is to leave some space in the bucket if we need to add new internal
    // fields to existing, full buckets.
    static constexpr int32_t largeMeasurementsMaxBucketSize =
        BSONObjMaxUserSize - (4 * 1024 * 1024);
    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize = std::min(largeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);

    bucket->_calculateBucketFieldsAndSizeChange(
        doc, info->options.getMetaField(), newFieldNamesToBeInserted, sizeToBeAdded);
    if (bucket->_size + *sizeToBeAdded > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket->_numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount) &&
            feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket->_size + *sizeToBeAdded > absoluteMaxSize) {
                if (absoluteMaxSize != largeMeasurementsMaxBucketSize) {
                    info->stats.incNumBucketsClosedDueToCachePressure();
                    return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
                }
                info->stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket->_keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket->_keptOpenDueToLargeMeasurements = true;
                info->stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }
            return {RolloverAction::kNone, RolloverReason::kNone};
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                info->stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }
            info->stats.incNumBucketsClosedDueToCachePressure();
            return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
        }
    }

    if (bucket->schemaIncompatible(
            doc, info->options.getMetaField(), info->key.metadata.getComparator())) {
        info->stats.incNumBucketsClosedDueToSchemaChange();
        return {RolloverAction::kHardClose, RolloverReason::kSchemaChange};
    }

    return {RolloverAction::kNone, RolloverReason::kNone};
}

BucketCatalog::Bucket* BucketCatalog::_rollover(Stripe* stripe,
                                                WithLock stripeLock,
                                                Bucket* bucket,
                                                const CreationInfo& info,
                                                RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (bucket->allCommitted()) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        if (action == RolloverAction::kArchive) {
            _archiveBucket(stripe, stripeLock, bucket, info.closedBuckets);
        } else {
            const bool eligibleForReopening = action == RolloverAction::kSoftClose;
            info.closedBuckets->emplace_back(ClosedBucket{&_bucketStateManager,
                                                          bucket->bucketId(),
                                                          bucket->getTimeField().toString(),
                                                          bucket->numMeasurements(),
                                                          eligibleForReopening});

            _removeBucket(stripe, stripeLock, bucket, RemovalMode::kClose);
        }
    } else {
        // We must keep the bucket around until all measurements are committed committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket->setRolloverAction(action);
    }

    return _allocateBucket(stripe, stripeLock, info);
}

BucketCatalog::ExecutionStatsController BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) {
    stdx::lock_guard catalogLock{_mutex};
    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return {it->second, &_globalExecutionStats};
    }

    auto res = _executionStats.emplace(ns, std::make_shared<ExecutionStats>());
    return {res.first->second, &_globalExecutionStats};
}

std::shared_ptr<BucketCatalog::ExecutionStats> BucketCatalog::_getExecutionStats(
    const NamespaceString& ns) const {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};

    stdx::lock_guard catalogLock{_mutex};

    auto it = _executionStats.find(ns);
    if (it != _executionStats.end()) {
        return it->second;
    }
    return kEmptyStats;
}

long long BucketCatalog::_marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                                               bool onlyEntryForMatchingMetaHash) {
    return sizeof(Date_t) +        // key in set of archived buckets for meta hash
        sizeof(ArchivedBucket) +   // main data for archived bucket
        bucket.timeField.size() +  // allocated space for timeField string, ignoring SSO
        (onlyEntryForMatchingMetaHash ? sizeof(std::size_t) +           // key in set (meta hash)
                 sizeof(decltype(Stripe::archivedBuckets)::value_type)  // set container
                                      : 0);
}

void BucketCatalog::_updateBucketFetchAndQueryStats(ExecutionStatsController& stats,
                                                    const BucketFindResult& findResult) {
    if (findResult.fetchedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsFetched();
        } else {
            stats.incNumBucketFetchesFailed();
        }
    }

    if (findResult.queriedBucket) {
        if (findResult.bucketToReopen.has_value()) {
            stats.incNumBucketsQueried();
        } else {
            stats.incNumBucketQueriesFailed();
        }
    }
}

class BucketCatalog::ServerStatus : public ServerStatusSection {
    struct BucketCounts {
        BucketCounts& operator+=(const BucketCounts& other) {
            if (&other != this) {
                all += other.all;
                open += other.open;
                idle += other.idle;
            }
            return *this;
        }

        std::size_t all = 0;
        std::size_t open = 0;
        std::size_t idle = 0;
    };

    BucketCounts _getBucketCounts(const BucketCatalog& catalog) const {
        BucketCounts sum;
        for (auto const& stripe : catalog._stripes) {
            stdx::lock_guard stripeLock{stripe.mutex};
            sum += {stripe.allBuckets.size(), stripe.openBuckets.size(), stripe.idleBuckets.size()};
        }
        return sum;
    }

public:
    ServerStatus() : ServerStatusSection("bucketCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        const auto& bucketCatalog = BucketCatalog::get(opCtx);
        {
            stdx::lock_guard catalogLock{bucketCatalog._mutex};
            if (bucketCatalog._executionStats.empty()) {
                return {};
            }
        }

        auto counts = _getBucketCounts(bucketCatalog);
        auto numActive = bucketCatalog._numberOfActiveBuckets.load();
        BSONObjBuilder builder;
        builder.appendNumber("numBuckets", static_cast<long long>(numActive));
        builder.appendNumber("numOpenBuckets", static_cast<long long>(counts.open));
        builder.appendNumber("numIdleBuckets", static_cast<long long>(counts.idle));
        builder.appendNumber("numArchivedBuckets", static_cast<long long>(numActive - counts.open));
        builder.appendNumber("memoryUsage",
                             static_cast<long long>(bucketCatalog._memoryUsage.load()));

        // Append the global execution stats for all namespaces.
        bucketCatalog.appendGlobalExecutionStats(&builder);

        // Append the global state management stats for all namespaces.
        bucketCatalog.appendStateManagementStats(&builder);

        return builder.obj();
    }
} bucketCatalogServerStatus;
}  // namespace mongo

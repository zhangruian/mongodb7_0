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

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"

#include <cstdint>

#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo {

namespace {
constexpr int32_t kPlaceholderInt = 0;
constexpr int64_t kPlaceholderLong = 0;
}  // namespace

namespace {
constexpr auto kResharding = "resharding";
constexpr auto kGlobalIndex = "globalIndex";
constexpr auto kCountStarted = "countStarted";
constexpr auto kCountSucceeded = "countSucceeded";
constexpr auto kCountFailed = "countFailed";
constexpr auto kCountCanceled = "countCanceled";
constexpr auto kLastOpEndingChunkImbalance = "lastOpEndingChunkImbalance";
constexpr auto kActive = "active";
constexpr auto kDocumentsProcessed = "documentsProcessed";
constexpr auto kBytesWritten = "bytesWritten";
constexpr auto kOplogEntriesFetched = "oplogEntriesFetched";
constexpr auto kOplogEntriesApplied = "oplogEntriesApplied";
constexpr auto kInsertsApplied = "insertsApplied";
constexpr auto kUpdatesApplied = "updatesApplied";
constexpr auto kDeletesApplied = "deletesApplied";
constexpr auto kCountWritesToStashCollections = "countWritesToStashCollections";
constexpr auto kCountWritesDuringCriticalSection = "countWritesDuringCriticalSection";
constexpr auto kCountReadsDuringCriticalSection = "countReadsDuringCriticalSection";
constexpr auto kOldestActive = "oldestActive";
constexpr auto kCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis =
    "coordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis";
constexpr auto kCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis =
    "coordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis";
constexpr auto kRecipientRemainingOperationTimeEstimatedMillis =
    "recipientRemainingOperationTimeEstimatedMillis";
constexpr auto kLatencies = "latencies";
constexpr auto kCollectionCloningTotalRemoteBatchRetrievalTimeMillis =
    "collectionCloningTotalRemoteBatchRetrievalTimeMillis";
constexpr auto kCollectionCloningTotalRemoteBatchesRetrieved =
    "collectionCloningTotalRemoteBatchesRetrieved";
constexpr auto kCollectionCloningTotalLocalInsertTimeMillis =
    "collectionCloningTotalLocalInsertTimeMillis";
constexpr auto kCollectionCloningTotalLocalInserts = "collectionCloningTotalLocalInserts";
constexpr auto kOplogFetchingTotalRemoteBatchRetrievalTimeMillis =
    "oplogFetchingTotalRemoteBatchRetrievalTimeMillis";
constexpr auto kOplogFetchingTotalRemoteBatchesRetrieved =
    "oplogFetchingTotalRemoteBatchesRetrieved";
constexpr auto kOplogFetchingTotalLocalInsertTimeMillis = "oplogFetchingTotalLocalInsertTimeMillis";
constexpr auto kOplogFetchingTotalLocalInserts = "oplogFetchingTotalLocalInserts";
constexpr auto kOplogApplyingTotalLocalBatchRetrievalTimeMillis =
    "oplogApplyingTotalLocalBatchRetrievalTimeMillis";
constexpr auto kOplogApplyingTotalLocalBatchesRetrieved = "oplogApplyingTotalLocalBatchesRetrieved";
constexpr auto kOplogApplyingTotalLocalBatchApplyTimeMillis =
    "oplogApplyingTotalLocalBatchApplyTimeMillis";
constexpr auto kOplogApplyingTotalLocalBatchesApplied = "oplogApplyingTotalLocalBatchesApplied";
constexpr auto kCurrentInSteps = "currentInSteps";
constexpr auto kCountInstancesInCoordinatorState1Initializing =
    "countInstancesInCoordinatorState1Initializing";
constexpr auto kCountInstancesInCoordinatorState2PreparingToDonate =
    "countInstancesInCoordinatorState2PreparingToDonate";
constexpr auto kCountInstancesInCoordinatorState3Cloning =
    "countInstancesInCoordinatorState3Cloning";
constexpr auto kCountInstancesInCoordinatorState4Applying =
    "countInstancesInCoordinatorState4Applying";
constexpr auto kCountInstancesInCoordinatorState5BlockingWrites =
    "countInstancesInCoordinatorState5BlockingWrites";
constexpr auto kCountInstancesInCoordinatorState6Aborting =
    "countInstancesInCoordinatorState6Aborting";
constexpr auto kCountInstancesInCoordinatorState7Committing =
    "countInstancesInCoordinatorState7Committing";
constexpr auto kCountInstancesInRecipientState1AwaitingFetchTimestamp =
    "countInstancesInRecipientState1AwaitingFetchTimestamp";
constexpr auto kCountInstancesInRecipientState2CreatingCollection =
    "countInstancesInRecipientState2CreatingCollection";
constexpr auto kCountInstancesInRecipientState3Cloning = "countInstancesInRecipientState3Cloning";
constexpr auto kCountInstancesInRecipientState4Applying = "countInstancesInRecipientState4Applying";
constexpr auto kCountInstancesInRecipientState5Error = "countInstancesInRecipientState5Error";
constexpr auto kCountInstancesInRecipientState6StrictConsistency =
    "countInstancesInRecipientState6StrictConsistency";
constexpr auto kCountInstancesInRecipientState7Done = "countInstancesInRecipientState7Done";
constexpr auto kCountInstancesInDonorState1PreparingToDonate =
    "countInstancesInDonorState1PreparingToDonate";
constexpr auto kCountInstancesInDonorState2DonatingInitialData =
    "countInstancesInDonorState2DonatingInitialData";
constexpr auto kCountInstancesInDonorState3DonatingOplogEntries =
    "countInstancesInDonorState3DonatingOplogEntries";
constexpr auto kCountInstancesInDonorState4PreparingToBlockWrites =
    "countInstancesInDonorState4PreparingToBlockWrites";
constexpr auto kCountInstancesInDonorState5Error = "countInstancesInDonorState5Error";
constexpr auto kCountInstancesInDonorState6BlockingWrites =
    "countInstancesInDonorState6BlockingWrites";
constexpr auto kCountInstancesInDonorState7Done = "countInstancesInDonorState7Done";

struct Metrics {
    Metrics() : _resharding(kResharding), _globalIndexes(kGlobalIndex) {}
    ShardingDataTransformCumulativeMetrics _resharding;
    ShardingDataTransformCumulativeMetrics _globalIndexes;
};
using MetricsPtr = std::unique_ptr<Metrics>;
const auto getMetrics = ServiceContext::declareDecoration<MetricsPtr>();

const auto metricsRegisterer = ServiceContext::ConstructorActionRegisterer{
    "ShardingDataTransformMetrics",
    [](ServiceContext* ctx) { getMetrics(ctx) = std::make_unique<Metrics>(); }};

}  // namespace

ShardingDataTransformCumulativeMetrics* ShardingDataTransformCumulativeMetrics::getForResharding(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_resharding;
}

ShardingDataTransformCumulativeMetrics* ShardingDataTransformCumulativeMetrics::getForGlobalIndexes(
    ServiceContext* context) {
    auto& metrics = getMetrics(context);
    return &metrics->_globalIndexes;
}

ShardingDataTransformCumulativeMetrics::ShardingDataTransformCumulativeMetrics(
    const std::string& rootSectionName)
    : _rootSectionName{rootSectionName},
      _instanceMetricsForAllRoles(ShardingDataTransformMetrics::kRoleCount),
      _operationWasAttempted{false},
      _coordinatorStateList{AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0},
                            AtomicWord<int64_t>{0}},
      _donorStateList{AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0},
                      AtomicWord<int64_t>{0}},
      _recipientStateList{AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0},
                          AtomicWord<int64_t>{0}} {}

ShardingDataTransformCumulativeMetrics::DeregistrationFunction
ShardingDataTransformCumulativeMetrics::registerInstanceMetrics(const InstanceObserver* metrics) {
    _operationWasAttempted.store(true);
    auto role = metrics->getRole();
    auto it = insertMetrics(metrics, getMetricsSetForRole(role));
    return [=] {
        stdx::unique_lock guard(_mutex);
        getMetricsSetForRole(role).erase(it);
    };
}

int64_t ShardingDataTransformCumulativeMetrics::getOldestOperationHighEstimateRemainingTimeMillis(
    Role role) const {

    stdx::unique_lock guard(_mutex);
    auto op = getOldestOperation(guard, role);
    return op ? op->getHighEstimateRemainingTimeMillis() : 0;
}

int64_t ShardingDataTransformCumulativeMetrics::getOldestOperationLowEstimateRemainingTimeMillis(
    Role role) const {

    stdx::unique_lock guard(_mutex);
    auto op = getOldestOperation(guard, role);
    return op ? op->getLowEstimateRemainingTimeMillis() : 0;
}

size_t ShardingDataTransformCumulativeMetrics::getObservedMetricsCount() const {
    stdx::unique_lock guard(_mutex);
    size_t count = 0;
    for (const auto& set : _instanceMetricsForAllRoles) {
        count += set.size();
    }
    return count;
}

size_t ShardingDataTransformCumulativeMetrics::getObservedMetricsCount(Role role) const {
    stdx::unique_lock guard(_mutex);
    return getMetricsSetForRole(role).size();
}

void ShardingDataTransformCumulativeMetrics::reportForServerStatus(BSONObjBuilder* bob) const {
    if (!_operationWasAttempted.load()) {
        return;
    }

    BSONObjBuilder root(bob->subobjStart(_rootSectionName));
    root.append(kCountStarted, _countStarted.load());
    root.append(kCountSucceeded, _countSucceeded.load());
    root.append(kCountFailed, _countFailed.load());
    root.append(kCountCanceled, _countCancelled.load());
    root.append(kLastOpEndingChunkImbalance, _lastOpEndingChunkImbalance.load());

    reportActive(&root);
    reportOldestActive(&root);
    reportLatencies(&root);
    reportCurrentInSteps(&root);
}

void ShardingDataTransformCumulativeMetrics::reportActive(BSONObjBuilder* bob) const {
    BSONObjBuilder s(bob->subobjStart(kActive));
    s.append(kDocumentsProcessed, kPlaceholderLong);
    s.append(kBytesWritten, kPlaceholderLong);
    s.append(kOplogEntriesFetched, kPlaceholderLong);
    s.append(kOplogEntriesApplied, kPlaceholderLong);
    s.append(kInsertsApplied, kPlaceholderLong);
    s.append(kUpdatesApplied, kPlaceholderLong);
    s.append(kDeletesApplied, kPlaceholderLong);
    s.append(kCountWritesToStashCollections, _writesToStashedCollections.load());
    s.append(kCountWritesDuringCriticalSection, _writesDuringCriticalSection.load());
    s.append(kCountReadsDuringCriticalSection, _readsDuringCriticalSection.load());
}

void ShardingDataTransformCumulativeMetrics::reportOldestActive(BSONObjBuilder* bob) const {
    BSONObjBuilder s(bob->subobjStart(kOldestActive));
    s.append(kCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis,
             getOldestOperationHighEstimateRemainingTimeMillis(Role::kCoordinator));
    s.append(kCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis,
             getOldestOperationLowEstimateRemainingTimeMillis(Role::kCoordinator));
    s.append(kRecipientRemainingOperationTimeEstimatedMillis,
             getOldestOperationHighEstimateRemainingTimeMillis(Role::kRecipient));
}

void ShardingDataTransformCumulativeMetrics::reportLatencies(BSONObjBuilder* bob) const {
    BSONObjBuilder s(bob->subobjStart(kLatencies));
    s.append(kCollectionCloningTotalRemoteBatchRetrievalTimeMillis, kPlaceholderLong);
    s.append(kCollectionCloningTotalRemoteBatchesRetrieved, kPlaceholderLong);
    s.append(kCollectionCloningTotalLocalInsertTimeMillis,
             _collectionCloningTotalLocalInsertTimeMillis.load());
    s.append(kCollectionCloningTotalLocalInserts, _collectionCloningTotalLocalInserts.load());
    s.append(kOplogFetchingTotalRemoteBatchRetrievalTimeMillis,
             _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.load());
    s.append(kOplogFetchingTotalRemoteBatchesRetrieved,
             _oplogFetchingTotalRemoteBatchesRetrieved.load());
    s.append(kOplogFetchingTotalLocalInsertTimeMillis,
             _oplogFetchingTotalLocalInsertTimeMillis.load());
    s.append(kOplogFetchingTotalLocalInserts, _oplogFetchingTotalLocalInserts.load());
    s.append(kOplogApplyingTotalLocalBatchRetrievalTimeMillis,
             _oplogApplyingTotalBatchesRetrievalTimeMillis.load());
    s.append(kOplogApplyingTotalLocalBatchesRetrieved, _oplogApplyingTotalBatchesRetrieved.load());
    s.append(kOplogApplyingTotalLocalBatchApplyTimeMillis, kPlaceholderLong);
    s.append(kOplogApplyingTotalLocalBatchesApplied, kPlaceholderLong);
}

void ShardingDataTransformCumulativeMetrics::reportCurrentInSteps(BSONObjBuilder* bob) const {
    BSONObjBuilder s(bob->subobjStart(kCurrentInSteps));

    auto reportState = [this, &s](auto state) {
        s.append(fieldNameFor(state), getStateCounter(state)->load());
    };

    reportState(CoordinatorStateEnum::kInitializing);
    reportState(CoordinatorStateEnum::kPreparingToDonate);
    reportState(CoordinatorStateEnum::kCloning);
    reportState(CoordinatorStateEnum::kApplying);
    reportState(CoordinatorStateEnum::kBlockingWrites);
    reportState(CoordinatorStateEnum::kAborting);
    reportState(CoordinatorStateEnum::kCommitting);

    reportState(RecipientStateEnum::kAwaitingFetchTimestamp);
    reportState(RecipientStateEnum::kCreatingCollection);
    reportState(RecipientStateEnum::kCloning);
    reportState(RecipientStateEnum::kApplying);
    reportState(RecipientStateEnum::kError);
    reportState(RecipientStateEnum::kStrictConsistency);
    reportState(RecipientStateEnum::kDone);

    reportState(DonorStateEnum::kPreparingToDonate);
    reportState(DonorStateEnum::kDonatingInitialData);
    reportState(DonorStateEnum::kDonatingOplogEntries);
    reportState(DonorStateEnum::kPreparingToBlockWrites);
    reportState(DonorStateEnum::kError);
    reportState(DonorStateEnum::kBlockingWrites);
    reportState(DonorStateEnum::kDone);
}

const ShardingDataTransformCumulativeMetrics::InstanceObserver*
ShardingDataTransformCumulativeMetrics::getOldestOperation(WithLock, Role role) const {
    auto set = getMetricsSetForRole(role);
    if (set.empty()) {
        return nullptr;
    }
    return *set.begin();
}

ShardingDataTransformCumulativeMetrics::MetricsSet&
ShardingDataTransformCumulativeMetrics::getMetricsSetForRole(Role role) {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

const ShardingDataTransformCumulativeMetrics::MetricsSet&
ShardingDataTransformCumulativeMetrics::getMetricsSetForRole(Role role) const {
    return _instanceMetricsForAllRoles[static_cast<size_t>(role)];
}

ShardingDataTransformCumulativeMetrics::MetricsSet::iterator
ShardingDataTransformCumulativeMetrics::insertMetrics(const InstanceObserver* metrics,
                                                      MetricsSet& set) {
    stdx::unique_lock guard(_mutex);
    auto before = set.size();
    auto it = set.insert(set.end(), metrics);
    invariant(before + 1 == set.size());
    return it;
}

void ShardingDataTransformCumulativeMetrics::onStarted() {
    _countStarted.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onCompletion(ReshardingOperationStatusEnum status) {
    switch (status) {
        case ReshardingOperationStatusEnum::kSuccess:
            _countSucceeded.fetchAndAdd(1);
            break;
        case ReshardingOperationStatusEnum::kFailure:
            _countFailed.fetchAndAdd(1);
            break;
        case ReshardingOperationStatusEnum::kCanceled:
            _countCancelled.fetchAndAdd(1);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void ShardingDataTransformCumulativeMetrics::setLastOpEndingChunkImbalance(int64_t imbalanceCount) {
    _lastOpEndingChunkImbalance.store(imbalanceCount);
}

ShardingDataTransformCumulativeMetrics::CoordinatorStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(CoordinatorStateEnum state) {
    return &_coordinatorStateList;
}

const ShardingDataTransformCumulativeMetrics::CoordinatorStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(CoordinatorStateEnum state) const {
    return &_coordinatorStateList;
}

ShardingDataTransformCumulativeMetrics::DonorStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(DonorStateEnum state) {
    return &_donorStateList;
}

const ShardingDataTransformCumulativeMetrics::DonorStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(DonorStateEnum state) const {
    return &_donorStateList;
}

const char* ShardingDataTransformCumulativeMetrics::fieldNameFor(
    ShardingDataTransformCumulativeMetrics::CoordinatorStateEnum state) {
    switch (state) {
        case CoordinatorStateEnum::kInitializing:
            return kCountInstancesInCoordinatorState1Initializing;

        case CoordinatorStateEnum::kPreparingToDonate:
            return kCountInstancesInCoordinatorState2PreparingToDonate;

        case CoordinatorStateEnum::kCloning:
            return kCountInstancesInCoordinatorState3Cloning;

        case CoordinatorStateEnum::kApplying:
            return kCountInstancesInCoordinatorState4Applying;

        case CoordinatorStateEnum::kBlockingWrites:
            return kCountInstancesInCoordinatorState5BlockingWrites;

        case CoordinatorStateEnum::kAborting:
            return kCountInstancesInCoordinatorState6Aborting;

        case CoordinatorStateEnum::kCommitting:
            return kCountInstancesInCoordinatorState7Committing;

        default:
            uasserted(6438601,
                      str::stream()
                          << "no field name for coordinator state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

void ShardingDataTransformCumulativeMetrics::onInsertsDuringCloning(
    int64_t count, const Milliseconds& elapsedTime) {
    _collectionCloningTotalLocalInserts.fetchAndAdd(count);
    _collectionCloningTotalLocalInsertTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

void ShardingDataTransformCumulativeMetrics::onRemoteBatchRetrievedDuringOplogFetching(
    int64_t count, const Milliseconds& elapsedTime) {
    _oplogFetchingTotalRemoteBatchesRetrieved.fetchAndAdd(count);
    _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

void ShardingDataTransformCumulativeMetrics::onLocalInsertDuringOplogFetching(
    const Milliseconds& elapsedTime) {
    _oplogFetchingTotalLocalInserts.fetchAndAdd(1);
    _oplogFetchingTotalLocalInsertTimeMillis.fetchAndAdd(durationCount<Milliseconds>(elapsedTime));
}

void ShardingDataTransformCumulativeMetrics::onBatchRetrievedDuringOplogApplying(
    int64_t count, const Milliseconds& elapsedTime) {
    _oplogApplyingTotalBatchesRetrieved.fetchAndAdd(count);
    _oplogApplyingTotalBatchesRetrievalTimeMillis.fetchAndAdd(
        durationCount<Milliseconds>(elapsedTime));
}

const char* ShardingDataTransformCumulativeMetrics::fieldNameFor(
    ShardingDataTransformCumulativeMetrics::DonorStateEnum state) {
    switch (state) {
        case DonorStateEnum::kPreparingToDonate:
            return kCountInstancesInDonorState1PreparingToDonate;

        case DonorStateEnum::kDonatingInitialData:
            return kCountInstancesInDonorState2DonatingInitialData;

        case DonorStateEnum::kDonatingOplogEntries:
            return kCountInstancesInDonorState3DonatingOplogEntries;

        case DonorStateEnum::kPreparingToBlockWrites:
            return kCountInstancesInDonorState4PreparingToBlockWrites;

        case DonorStateEnum::kError:
            return kCountInstancesInDonorState5Error;

        case DonorStateEnum::kBlockingWrites:
            return kCountInstancesInDonorState6BlockingWrites;

        case DonorStateEnum::kDone:
            return kCountInstancesInDonorState7Done;

        default:
            uasserted(6438700,
                      str::stream()
                          << "no field name for donor state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

void ShardingDataTransformCumulativeMetrics::onReadDuringCriticalSection() {
    _readsDuringCriticalSection.fetchAndAdd(1);
}

void ShardingDataTransformCumulativeMetrics::onWriteDuringCriticalSection() {
    _writesDuringCriticalSection.fetchAndAdd(1);
}

const char* ShardingDataTransformCumulativeMetrics::fieldNameFor(RecipientStateEnum state) {
    switch (state) {
        case RecipientStateEnum::kAwaitingFetchTimestamp:
            return kCountInstancesInRecipientState1AwaitingFetchTimestamp;

        case RecipientStateEnum::kCreatingCollection:
            return kCountInstancesInRecipientState2CreatingCollection;

        case RecipientStateEnum::kCloning:
            return kCountInstancesInRecipientState3Cloning;

        case RecipientStateEnum::kApplying:
            return kCountInstancesInRecipientState4Applying;

        case RecipientStateEnum::kError:
            return kCountInstancesInRecipientState5Error;

        case RecipientStateEnum::kStrictConsistency:
            return kCountInstancesInRecipientState6StrictConsistency;

        case RecipientStateEnum::kDone:
            return kCountInstancesInRecipientState7Done;

        default:
            uasserted(6438900,
                      str::stream()
                          << "no field name for recipient state " << static_cast<int32_t>(state));
            break;
    }

    MONGO_UNREACHABLE;
}

void ShardingDataTransformCumulativeMetrics::onWriteToStashedCollections() {
    _writesToStashedCollections.fetchAndAdd(1);
}

ShardingDataTransformCumulativeMetrics::RecipientStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(
    ShardingDataTransformCumulativeMetrics::RecipientStateEnum state) {
    return &_recipientStateList;
}

const ShardingDataTransformCumulativeMetrics::RecipientStateArray*
ShardingDataTransformCumulativeMetrics::getStateArrayFor(
    ShardingDataTransformCumulativeMetrics::RecipientStateEnum state) const {
    return &_recipientStateList;
}

}  // namespace mongo

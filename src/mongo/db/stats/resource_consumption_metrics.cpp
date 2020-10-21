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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include <cmath>

#include "mongo/db/stats/resource_consumption_metrics.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const OperationContext::Decoration<ResourceConsumption::MetricsCollector> getMetricsCollector =
    OperationContext::declareDecoration<ResourceConsumption::MetricsCollector>();
const ServiceContext::Decoration<ResourceConsumption> getGlobalResourceConsumption =
    ServiceContext::declareDecoration<ResourceConsumption>();

static const char kPrimaryMetrics[] = "primaryMetrics";
static const char kSecondaryMetrics[] = "secondaryMetrics";
static const char kDocBytesRead[] = "docBytesRead";
static const char kDocUnitsRead[] = "docUnitsRead";
static const char kIdxEntryBytesRead[] = "idxEntryBytesRead";
static const char kIdxEntryUnitsRead[] = "idxEntryUnitsRead";
static const char kKeysSorted[] = "keysSorted";
static const char kCpuMillis[] = "cpuMillis";
static const char kDocBytesWritten[] = "docBytesWritten";
static const char kDocUnitsWritten[] = "docUnitsWritten";
static const char kIdxEntryBytesWritten[] = "idxEntryBytesWritten";
static const char kIdxEntryUnitsWritten[] = "idxEntryUnitsWritten";
static const char kDocUnitsReturned[] = "docUnitsReturned";

inline void appendNonZeroMetric(BSONObjBuilder* builder, const char* name, long long value) {
    if (value != 0) {
        builder->append(name, value);
    }
}
}  // namespace

bool ResourceConsumption::isMetricsCollectionEnabled() {
    return gMeasureOperationResourceConsumption.isEnabledAndIgnoreFCV();
}

bool ResourceConsumption::isMetricsAggregationEnabled() {
    return gAggregateOperationResourceConsumptionMetrics;
}

ResourceConsumption::ResourceConsumption() {
    if (gAggregateOperationResourceConsumptionMetrics &&
        !gMeasureOperationResourceConsumption.isEnabledAndIgnoreFCV()) {
        LOGV2_FATAL_NOTRACE(
            5091600,
            "measureOperationResourceConsumption feature flag must be enabled to use "
            "aggregateOperationResourceConsumptionMetrics");
    }
}

ResourceConsumption::MetricsCollector& ResourceConsumption::MetricsCollector::get(
    OperationContext* opCtx) {
    return getMetricsCollector(opCtx);
}

void ResourceConsumption::Metrics::toBson(BSONObjBuilder* builder) const {
    {
        BSONObjBuilder primaryBuilder = builder->subobjStart(kPrimaryMetrics);
        primaryBuilder.appendNumber(kDocBytesRead, primaryMetrics.docBytesRead);
        primaryBuilder.appendNumber(kDocUnitsRead, primaryMetrics.docUnitsRead);
        primaryBuilder.appendNumber(kIdxEntryBytesRead, primaryMetrics.idxEntryBytesRead);
        primaryBuilder.appendNumber(kIdxEntryUnitsRead, primaryMetrics.idxEntryUnitsRead);
        primaryBuilder.appendNumber(kKeysSorted, primaryMetrics.keysSorted);
        primaryBuilder.appendNumber(kDocUnitsReturned, primaryMetrics.docUnitsReturned);
        primaryBuilder.done();
    }

    {
        BSONObjBuilder secondaryBuilder = builder->subobjStart(kSecondaryMetrics);
        secondaryBuilder.appendNumber(kDocBytesRead, secondaryMetrics.docBytesRead);
        secondaryBuilder.appendNumber(kDocUnitsRead, secondaryMetrics.docUnitsRead);
        secondaryBuilder.appendNumber(kIdxEntryBytesRead, secondaryMetrics.idxEntryBytesRead);
        secondaryBuilder.appendNumber(kIdxEntryUnitsRead, secondaryMetrics.idxEntryUnitsRead);
        secondaryBuilder.appendNumber(kKeysSorted, secondaryMetrics.keysSorted);
        secondaryBuilder.appendNumber(kDocUnitsReturned, secondaryMetrics.docUnitsReturned);
        secondaryBuilder.done();
    }

    builder->appendNumber(kCpuMillis, cpuMillis);
    builder->appendNumber(kDocBytesWritten, docBytesWritten);
    builder->appendNumber(kDocUnitsWritten, docUnitsWritten);
    builder->appendNumber(kIdxEntryBytesWritten, idxEntryBytesWritten);
    builder->appendNumber(kIdxEntryUnitsWritten, idxEntryUnitsWritten);
}

void ResourceConsumption::Metrics::toFlatBsonAllFields(BSONObjBuilder* builder) const {
    // Report all read metrics together to generate a flat object.
    auto readMetrics = primaryMetrics + secondaryMetrics;
    builder->appendNumber(kDocBytesRead, readMetrics.docBytesRead);
    builder->appendNumber(kDocUnitsRead, readMetrics.docUnitsRead);
    builder->appendNumber(kIdxEntryBytesRead, readMetrics.idxEntryBytesRead);
    builder->appendNumber(kIdxEntryUnitsRead, readMetrics.idxEntryUnitsRead);
    builder->appendNumber(kKeysSorted, readMetrics.keysSorted);
    builder->appendNumber(kDocUnitsReturned, readMetrics.docUnitsReturned);

    builder->appendNumber(kCpuMillis, cpuMillis);
    builder->appendNumber(kDocBytesWritten, docBytesWritten);
    builder->appendNumber(kDocUnitsWritten, docUnitsWritten);
    builder->appendNumber(kIdxEntryBytesWritten, idxEntryBytesWritten);
    builder->appendNumber(kIdxEntryUnitsWritten, idxEntryUnitsWritten);
}

void ResourceConsumption::Metrics::toFlatBsonNonZeroFields(BSONObjBuilder* builder) const {
    // Report all read metrics together to generate a flat object.
    auto readMetrics = primaryMetrics + secondaryMetrics;
    appendNonZeroMetric(builder, kDocBytesRead, readMetrics.docBytesRead);
    appendNonZeroMetric(builder, kDocUnitsRead, readMetrics.docUnitsRead);
    appendNonZeroMetric(builder, kIdxEntryBytesRead, readMetrics.idxEntryBytesRead);
    appendNonZeroMetric(builder, kIdxEntryUnitsRead, readMetrics.idxEntryUnitsRead);
    appendNonZeroMetric(builder, kKeysSorted, readMetrics.keysSorted);
    appendNonZeroMetric(builder, kDocUnitsReturned, readMetrics.docUnitsReturned);

    appendNonZeroMetric(builder, kCpuMillis, cpuMillis);
    appendNonZeroMetric(builder, kDocBytesWritten, docBytesWritten);
    appendNonZeroMetric(builder, kDocUnitsWritten, docUnitsWritten);
    appendNonZeroMetric(builder, kIdxEntryBytesWritten, idxEntryBytesWritten);
    appendNonZeroMetric(builder, kIdxEntryUnitsWritten, idxEntryUnitsWritten);
}

template <typename Func>
inline void ResourceConsumption::MetricsCollector::_doIfCollecting(Func&& func) {
    if (!isCollecting()) {
        return;
    }
    func();
}

void ResourceConsumption::MetricsCollector::_updateReadMetrics(OperationContext* opCtx,
                                                               ReadMetricsFunc&& updateFunc) {
    _doIfCollecting([&] {
        // The RSTL is normally required to check the replication state, but callers may not always
        // be holding it. Since we need to attribute this metric to some replication state, and an
        // inconsistent state is not impactful for the purposes of metrics collection, perform a
        // best-effort check so that we can record metrics for this operation.
        if (repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase_UNSAFE(
                opCtx, NamespaceString::kAdminDb)) {
            updateFunc(_metrics.primaryMetrics);
        } else {
            updateFunc(_metrics.secondaryMetrics);
        }
    });
}

void ResourceConsumption::MetricsCollector::incrementOneDocRead(OperationContext* opCtx,
                                                                size_t docBytesRead) {
    _updateReadMetrics(opCtx, [&](ReadMetrics& readMetrics) {
        size_t docUnits = std::ceil(docBytesRead / static_cast<float>(gDocumentUnitSizeBytes));
        readMetrics.docBytesRead += docBytesRead;
        readMetrics.docUnitsRead += docUnits;
    });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryRead(OperationContext* opCtx,
                                                                     size_t bytesRead) {
    _updateReadMetrics(opCtx, [&](ReadMetrics& readMetrics) {
        size_t units = std::ceil(bytesRead / static_cast<float>(gIndexEntryUnitSizeBytes));
        readMetrics.idxEntryBytesRead += bytesRead;
        readMetrics.idxEntryUnitsRead += units;
    });
}

void ResourceConsumption::MetricsCollector::incrementKeysSorted(OperationContext* opCtx,
                                                                size_t keysSorted) {
    _updateReadMetrics(opCtx,
                       [&](ReadMetrics& readMetrics) { readMetrics.keysSorted += keysSorted; });
}

void ResourceConsumption::MetricsCollector::incrementDocUnitsReturned(OperationContext* opCtx,
                                                                      size_t returned) {
    _updateReadMetrics(opCtx,
                       [&](ReadMetrics& readMetrics) { readMetrics.docUnitsReturned += returned; });
}

void ResourceConsumption::MetricsCollector::incrementOneDocWritten(size_t bytesWritten) {
    _doIfCollecting([&] {
        size_t docUnits = std::ceil(bytesWritten / static_cast<float>(gDocumentUnitSizeBytes));
        _metrics.docBytesWritten += bytesWritten;
        _metrics.docUnitsWritten += docUnits;
    });
}

void ResourceConsumption::MetricsCollector::incrementOneIdxEntryWritten(size_t bytesWritten) {
    _doIfCollecting([&] {
        size_t idxUnits = std::ceil(bytesWritten / static_cast<float>(gIndexEntryUnitSizeBytes));
        _metrics.idxEntryBytesWritten += bytesWritten;
        _metrics.idxEntryUnitsWritten += idxUnits;
    });
}

void ResourceConsumption::MetricsCollector::incrementCpuMillis(size_t cpuMillis) {
    _doIfCollecting([&] { _metrics.cpuMillis += cpuMillis; });
}

ResourceConsumption::ScopedMetricsCollector::ScopedMetricsCollector(OperationContext* opCtx,
                                                                    const std::string& dbName,
                                                                    bool commandCollectsMetrics)
    : _opCtx(opCtx) {

    // Nesting is allowed but does nothing. Lower-level ScopedMetricsCollectors should not influence
    // the top-level Collector's behavior.
    auto& metrics = MetricsCollector::get(opCtx);
    _topLevel = !metrics.isInScope();
    if (!_topLevel) {
        return;
    }

    if (!commandCollectsMetrics || !shouldCollectMetricsForDatabase(dbName) ||
        !isMetricsCollectionEnabled()) {
        metrics.beginScopedNotCollecting();
        return;
    }

    metrics.beginScopedCollecting(dbName);
}

ResourceConsumption::ScopedMetricsCollector::~ScopedMetricsCollector() {
    if (!_topLevel) {
        return;
    }

    auto& collector = MetricsCollector::get(_opCtx);
    bool wasCollecting = collector.endScopedCollecting();
    if (!wasCollecting) {
        return;
    }

    if (collector.getDbName().empty()) {
        return;
    }

    if (!isMetricsAggregationEnabled()) {
        return;
    }

    auto& globalResourceConsumption = ResourceConsumption::get(_opCtx);
    globalResourceConsumption.add(collector);
}

ResourceConsumption& ResourceConsumption::get(ServiceContext* svcCtx) {
    return getGlobalResourceConsumption(svcCtx);
}

ResourceConsumption& ResourceConsumption::get(OperationContext* opCtx) {
    return getGlobalResourceConsumption(opCtx->getServiceContext());
}

void ResourceConsumption::add(const MetricsCollector& collector) {
    invariant(!collector.getDbName().empty());
    stdx::unique_lock<Mutex> lk(_mutex);
    _metrics[collector.getDbName()] += collector.getMetrics();
}

ResourceConsumption::MetricsMap ResourceConsumption::getMetrics() const {
    stdx::unique_lock<Mutex> lk(_mutex);
    return _metrics;
}

ResourceConsumption::MetricsMap ResourceConsumption::getAndClearMetrics() {
    stdx::unique_lock<Mutex> lk(_mutex);
    MetricsMap newMap;
    _metrics.swap(newMap);
    return newMap;
}

}  // namespace mongo

/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/s/query_analysis_sample_counters.h"

#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/is_mongos.h"

namespace mongo {
namespace analyze_shard_key {
namespace {

const auto getQueryAnalysisSampleCounters =
    ServiceContext::declareDecoration<QueryAnalysisSampleCounters>();

}  // namespace

QueryAnalysisSampleCounters& QueryAnalysisSampleCounters::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisSampleCounters& QueryAnalysisSampleCounters::get(ServiceContext* serviceContext) {
    return getQueryAnalysisSampleCounters(serviceContext);
}

void QueryAnalysisSampleCounters::refreshConfigurations(
    const std::vector<CollectionQueryAnalyzerConfiguration>& configurations) {
    stdx::lock_guard<Latch> lk(_mutex);
    std::map<NamespaceString,
             std::shared_ptr<QueryAnalysisSampleCounters::CollectionSampleCounters>>
        newSampleCounters;

    for (const auto& configuration : configurations) {
        auto it = _sampleCounters.find(configuration.getNs());
        if (it == _sampleCounters.end() ||
            it->second->getCollUuid() != configuration.getCollectionUuid()) {
            newSampleCounters.emplace(std::make_pair(
                configuration.getNs(),
                std::make_shared<CollectionSampleCounters>(configuration.getNs(),
                                                           configuration.getCollectionUuid(),
                                                           configuration.getSampleRate())));
        } else {
            it->second->setSampleRate(configuration.getSampleRate());
            newSampleCounters.emplace(std::make_pair(configuration.getNs(), it->second));
        }
        _sampledNamespaces.insert(configuration.getNs());
    }
    _sampleCounters = std::move(newSampleCounters);
}

void QueryAnalysisSampleCounters::incrementReads(const NamespaceString& nss,
                                                 const boost::optional<UUID>& collUuid,
                                                 boost::optional<int64_t> size) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto counters = _getOrCreateCollectionSampleCounters(lk, nss, collUuid);
    counters->incrementReads(size);
    ++_totalSampledReadsCount;
    if (size) {
        _totalSampledReadsBytes += *size;
    }
}

void QueryAnalysisSampleCounters::incrementWrites(const NamespaceString& nss,
                                                  const boost::optional<UUID>& collUuid,
                                                  boost::optional<int64_t> size) {
    stdx::lock_guard<Latch> lk(_mutex);
    auto counters = _getOrCreateCollectionSampleCounters(lk, nss, collUuid);
    counters->incrementWrites(size);
    ++_totalSampledWritesCount;
    if (size) {
        _totalSampledWritesBytes += *size;
    }
}

std::shared_ptr<QueryAnalysisSampleCounters::CollectionSampleCounters>
QueryAnalysisSampleCounters::_getOrCreateCollectionSampleCounters(
    WithLock, const NamespaceString& nss, const boost::optional<UUID>& collUuid) {
    auto it = _sampleCounters.find(nss);
    if (it == _sampleCounters.end()) {
        // Do not create a new set of counters without collUuid specified:
        invariant(collUuid);
        it = _sampleCounters
                 .emplace(std::make_pair(
                     nss,
                     std::make_shared<QueryAnalysisSampleCounters::CollectionSampleCounters>(
                         nss, *collUuid)))
                 .first;
        _sampledNamespaces.insert(nss);
    }
    return it->second;
}

void QueryAnalysisSampleCounters::reportForCurrentOp(std::vector<BSONObj>* ops) const {
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto const& it : _sampleCounters) {
        ops->push_back(it.second->reportForCurrentOp());
    }
}

BSONObj QueryAnalysisSampleCounters::CollectionSampleCounters::reportForCurrentOp() const {
    CollectionSampleCountersCurrentOp report;
    report.setNs(_nss);
    report.setCollUuid(_collUuid);
    report.setSampledReadsCount(_sampledReadsCount);
    report.setSampledWritesCount(_sampledWritesCount);
    if (isMongos()) {
        report.setSampleRate(_sampleRate);
    } else {
        report.setSampledReadsBytes(_sampledReadsBytes);
        report.setSampledWritesBytes(_sampledWritesBytes);
    }

    return report.toBSON();
}

BSONObj QueryAnalysisSampleCounters::reportForServerStatus() const {
    QueryAnalysisServerStatus res;
    res.setActiveCollections(static_cast<int64_t>(_sampleCounters.size()));
    res.setTotalCollections(static_cast<int64_t>(_sampledNamespaces.size()));
    res.setTotalSampledReadsCount(_totalSampledReadsCount);
    res.setTotalSampledWritesCount(_totalSampledWritesCount);
    if (!isMongos()) {
        res.setTotalSampledReadsBytes(_totalSampledReadsBytes);
        res.setTotalSampledWritesBytes(_totalSampledWritesBytes);
    }
    return res.toBSON();
}

}  // namespace analyze_shard_key
}  // namespace mongo

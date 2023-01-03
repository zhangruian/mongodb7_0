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

#include "mongo/s/query_analysis_sampler_util.h"

#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr auto kSampleIdFieldName = "sampleId"_sd;

template <typename C>
auto sampleIter(C&& c) {
    static StaticImmortal<synchronized_value<PseudoRandom>> random{
        PseudoRandom{SecureRandom().nextInt64()}};
    return std::next(c.begin(), (*random)->nextInt64(c.size()));
}

}  // namespace

boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx, const NamespaceString& nss) {
    return supportsSamplingQueries() ? QueryAnalysisSampler::get(opCtx).tryGenerateSampleId(nss)
                                     : boost::none;
}

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const std::set<ShardId>& shardIds) {
    if (auto sampleId = tryGenerateSampleId(opCtx, nss)) {
        return TargetedSampleId{*sampleId, getRandomShardId(shardIds)};
    }
    return boost::none;
}

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ShardEndpoint>& endpoints) {
    if (auto sampleId = tryGenerateSampleId(opCtx, nss)) {
        return TargetedSampleId{*sampleId, getRandomShardId(endpoints)};
    }
    return boost::none;
}

ShardId getRandomShardId(const std::set<ShardId>& shardIds) {
    return *sampleIter(shardIds);
}

ShardId getRandomShardId(const std::vector<ShardEndpoint>& endpoints) {
    return sampleIter(endpoints)->shardName;
}

BSONObj appendSampleId(const BSONObj& cmdObj, const UUID& sampleId) {
    BSONObjBuilder bob(std::move(cmdObj));
    appendSampleId(&bob, sampleId);
    return bob.obj();
}

void appendSampleId(BSONObjBuilder* bob, const UUID& sampleId) {
    sampleId.appendToBuilder(bob, kSampleIdFieldName);
}

}  // namespace analyze_shard_key
}  // namespace mongo

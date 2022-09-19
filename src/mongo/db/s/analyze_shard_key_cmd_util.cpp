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

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/s/analyze_shard_key_cmd_util.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr StringData kGroupByKeyFieldName = "key"_sd;
constexpr StringData kNumDocsFieldName = "numDocs"_sd;
constexpr StringData kCardinalityFieldName = "cardinality"_sd;
constexpr StringData kFrequencyFieldName = "frequency"_sd;
constexpr StringData kIndexFieldName = "index"_sd;

const std::vector<double> kPercentiles{0.99, 0.95, 0.9, 0.8, 0.5};

/**
 * Performs a fast count to get the total number of documents in the collection.
 */
long long getNumDocuments(OperationContext* opCtx, const NamespaceString& nss) {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        // The ServiceEntryPoint expects the ReadConcernArgs to not be set.
        auto originalReadConcernArgs = repl::ReadConcernArgs::get(opCtx);
        repl::ReadConcernArgs::get(opCtx) = repl::ReadConcernArgs();
        ON_BLOCK_EXIT([&] { repl::ReadConcernArgs::get(opCtx) = originalReadConcernArgs; });

        auto opMsgRequest =
            OpMsgRequest::fromDBAndBody(nss.db(), BSON("clusterCount" << nss.coll()));
        auto requestMessage = opMsgRequest.serialize();
        auto dbResponse =
            ServiceEntryPointMongos::handleRequestImpl(opCtx, requestMessage).get(opCtx);
        auto cmdResponse = rpc::makeReply(&dbResponse.response)->getCommandReply();

        uassertStatusOK(getStatusFromCommandResult(cmdResponse));
        return cmdResponse.getField("n").exactNumberLong();
    } else {
        DBDirectClient client(opCtx);
        return client.count(nss, BSONObj());
    }
}

/**
 * Returns an aggregate command request for calculating the cardinality and frequency of the given
 * shard key.
 */
AggregateCommandRequest makeAggregateRequestForCardinalityAndFrequency(
    const NamespaceString& nss, const BSONObj& shardKey, const BSONObj& hintIndexKey) {
    std::vector<BSONObj> pipeline;

    pipeline.push_back(BSON("$project" << BSON("_id" << 0 << kGroupByKeyFieldName
                                                     << BSON("$meta"
                                                             << "indexKey"))));

    BSONObjBuilder groupByBuilder;
    int fieldNum = 0;
    for (const auto& element : shardKey) {
        const auto fieldName = element.fieldNameStringData();
        groupByBuilder.append(kGroupByKeyFieldName + std::to_string(fieldNum),
                              BSON("$getField" << BSON("field" << fieldName << "input"
                                                               << ("$" + kGroupByKeyFieldName))));
        fieldNum++;
    }
    pipeline.push_back(BSON("$group" << BSON("_id" << groupByBuilder.obj() << kFrequencyFieldName
                                                   << BSON("$sum" << 1))));

    pipeline.push_back(BSON("$project" << BSON("_id" << 0)));
    pipeline.push_back(BSON(
        "$setWindowFields" << BSON(
            "sortBy" << BSON(kFrequencyFieldName << 1) << "output"
                     << BSON(kNumDocsFieldName
                             << BSON("$sum" << ("$" + kFrequencyFieldName)) << kCardinalityFieldName
                             << BSON("$sum" << 1) << kIndexFieldName
                             << BSON("$sum" << 1 << "window"
                                            << BSON("documents" << BSON_ARRAY("unbounded"
                                                                              << "current")))))));

    BSONObjBuilder orBuilder;
    BSONArrayBuilder arrayBuilder(orBuilder.subarrayStart("$or"));
    for (const auto& percentile : kPercentiles) {
        arrayBuilder.append(
            BSON("$eq" << BSON_ARRAY(
                     ("$" + kIndexFieldName)
                     << BSON("$ceil" << BSON("$multiply" << BSON_ARRAY(
                                                 percentile << ("$" + kCardinalityFieldName)))))));
    }
    arrayBuilder.done();
    pipeline.push_back(BSON("$match" << BSON("$expr" << orBuilder.done())));

    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setHint(hintIndexKey);
    aggRequest.setAllowDiskUse(true);
    // Use readConcern "available" to avoid shard filtering since it is expensive.
    aggRequest.setReadConcern(
        BSON(repl::ReadConcernArgs::kLevelFieldName << repl::readConcernLevels::kAvailableName));

    return aggRequest;
}

/**
 * Runs the given aggregate command request and applies 'callbackFn' to each returned document. On a
 * sharded cluster, automatically retries on shard versioning errors. Does not support runnning
 * getMore commands for the aggregation.
 */
void runAggregate(OperationContext* opCtx,
                  const NamespaceString& nss,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn) {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        const auto& catalogCache = Grid::get(opCtx)->catalogCache();
        bool succeeded = false;

        while (true) {
            try {
                shardVersionRetry(opCtx, catalogCache, nss, "AnalyzeShardKeyAggregation"_sd, [&] {
                    BSONObjBuilder responseBuilder;
                    uassertStatusOK(
                        ClusterAggregate::runAggregate(opCtx,
                                                       ClusterAggregate::Namespaces{nss, nss},
                                                       aggRequest,
                                                       LiteParsedPipeline{aggRequest},
                                                       PrivilegeVector(),
                                                       &responseBuilder));
                    succeeded = true;
                    auto firstBatch = responseBuilder.obj().firstElement()["firstBatch"].Obj();
                    BSONObjIterator it(firstBatch);

                    while (it.more()) {
                        auto doc = it.next().Obj();
                        callbackFn(doc);
                    }
                });
                return;
            } catch (const DBException& ex) {
                if (ex.toStatus() == ErrorCodes::ShardNotFound) {
                    // 'callbackFn' should never trigger a ShardNotFound error. It is also incorrect
                    // to retry the aggregate command after some documents have already been
                    // processed.
                    invariant(!succeeded);

                    LOGV2(6875200,
                          "Failed to run aggregate command to analyze shard key",
                          "error"_attr = ex.toStatus());
                    continue;
                }
                throw;
            }
        }

    } else {
        DBDirectClient client(opCtx);
        auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
            &client, aggRequest, true /* secondaryOk */, false /* useExhaust*/));

        while (cursor->more()) {
            auto doc = cursor->next();
            callbackFn(doc);
        }
    }
}

struct IndexSpec {
    BSONObj keyPattern;
    bool isUnique;
};

/**
 * Returns the IndexSpec for the index that has the given shard key as a prefix, ignoring the index
 * type (i.e. hashed or range). To be used for finding the index that can be used as a hint for the
 * aggregate command for calculating the cardinality and frequency metrics (the aggregation pipeline
 * works with both the original field values or by the hashes of the field values). The index must
 * have simple collation since that is the only supported collation for shard key string fields
 * comparisons.
 */
boost::optional<IndexSpec> findCompatiblePrefixedIndex(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const IndexCatalog* indexCatalog,
                                                       const BSONObj& shardKey) {
    if (collection->isClustered()) {
        auto indexSpec = collection->getClusteredInfo()->getIndexSpec();
        auto indexKey = indexSpec.getKey();
        if (shardKey.isFieldNamePrefixOf(indexKey)) {
            tassert(6875201, "Expected clustered index to be unique", indexSpec.getUnique());
            return IndexSpec{indexKey, indexSpec.getUnique()};
        }
    }

    auto indexIterator =
        indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        auto indexEntry = indexIterator->next();
        auto indexDesc = indexEntry->descriptor();
        auto indexKey = indexDesc->keyPattern();
        if (indexDesc->collation().isEmpty() && !indexEntry->isMultikey(opCtx, collection) &&
            shardKey.isFieldNamePrefixOf(indexKey)) {
            return IndexSpec{indexKey, indexDesc->unique()};
        }
    }

    return boost::none;
}

struct CardinalityFrequencyMetricsBundle {
    long long numDocs = 0;
    long long cardinality = 0;
    PercentileMetrics frequency;
};

/**
 * Returns the cardinality and frequency of the given shard key.
 */
CardinalityFrequencyMetricsBundle calculateCardinalityAndFrequency(OperationContext* opCtx,
                                                                   const NamespaceString& nss,
                                                                   const BSONObj& shardKey,
                                                                   const BSONObj& hintIndexKey,
                                                                   bool isShardKeyUnique) {
    CardinalityFrequencyMetricsBundle bundle;

    if (isShardKeyUnique) {
        long long numDocs = getNumDocuments(opCtx, nss);

        bundle.numDocs = numDocs;
        bundle.cardinality = numDocs;
        bundle.frequency.setP99(1);
        bundle.frequency.setP95(1);
        bundle.frequency.setP90(1);
        bundle.frequency.setP80(1);
        bundle.frequency.setP50(1);

        return bundle;
    }

    auto aggRequest = makeAggregateRequestForCardinalityAndFrequency(nss, shardKey, hintIndexKey);
    runAggregate(opCtx, nss, aggRequest, [&](const BSONObj& doc) {
        auto numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        auto cardinality = doc.getField(kCardinalityFieldName).exactNumberLong();
        auto frequency = doc.getField(kFrequencyFieldName).exactNumberLong();
        auto index = doc.getField(kIndexFieldName).exactNumberLong();

        invariant(numDocs > 0);
        invariant(cardinality > 0);
        invariant(frequency > 0);

        if (bundle.numDocs == 0) {
            bundle.numDocs = numDocs;
        } else {
            invariant(bundle.numDocs == numDocs);
        }

        if (bundle.cardinality == 0) {
            bundle.cardinality = cardinality;
        } else {
            invariant(bundle.cardinality == cardinality);
        }

        if (index == std::ceil(0.99 * cardinality)) {
            bundle.frequency.setP99(frequency);
        }
        if (index == std::ceil(0.95 * cardinality)) {
            bundle.frequency.setP95(frequency);
        }
        if (index == std::ceil(0.9 * cardinality)) {
            bundle.frequency.setP90(frequency);
        }
        if (index == std::ceil(0.8 * cardinality)) {
            bundle.frequency.setP80(frequency);
        }
        if (index == std::ceil(0.5 * cardinality)) {
            bundle.frequency.setP50(frequency);
        }
    });

    uassert(ErrorCodes::InvalidOptions,
            "Cannot analyze the cardinality and frequency of a shard key for an empty collection",
            bundle.numDocs > 0);

    return bundle;
}

}  // namespace

KeyCharacteristicsMetrics calculateKeyCharacteristicsMetrics(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const KeyPattern& shardKey) {
    KeyCharacteristicsMetrics metrics;

    auto shardKeyBson = shardKey.toBSON();
    BSONObj indexKeyBson;
    {
        AutoGetCollectionForReadCommand collection(opCtx, nss);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze a shard key for a non-existing collection",
                collection);

        auto indexSpec = findCompatiblePrefixedIndex(
            opCtx, *collection, collection->getIndexCatalog(), shardKeyBson);

        if (!indexSpec) {
            return {};
        }

        indexKeyBson = indexSpec->keyPattern.getOwned();
        metrics.setIsUnique(shardKeyBson.nFields() == indexKeyBson.nFields() ? indexSpec->isUnique
                                                                             : false);
    }

    auto bundle = calculateCardinalityAndFrequency(
        opCtx, nss, shardKeyBson, indexKeyBson, *metrics.getIsUnique());
    metrics.setNumDocs(bundle.numDocs);
    metrics.setCardinality(bundle.cardinality);
    metrics.setFrequency(bundle.frequency);

    return metrics;
}

}  // namespace analyze_shard_key
}  // namespace mongo

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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding_util.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
using namespace fmt::literals;

namespace {

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

    auto uuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    invariant(uuid);

    return *uuid;
}

// Ensure that this shard owns the document. This must be called after verifying that we
// are in a resharding operation so that we are guaranteed that migrations are suspended.
bool documentBelongsToMe(OperationContext* opCtx,
                         CollectionShardingState* css,
                         const ScopedCollectionDescription& collDesc,
                         const BSONObj& doc) {
    auto currentKeyPattern = ShardKeyPattern(collDesc.getKeyPattern());
    auto ownershipFilter = css->getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

    return ownershipFilter.keyBelongsToMe(currentKeyPattern.extractShardKeyFromDoc(doc));
}
}  // namespace

DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp,
                               boost::optional<Status> abortReason) {
    DonorShardContext donorCtx;
    donorCtx.setState(donorState);
    emplaceMinFetchTimestampIfExists(donorCtx, minFetchTimestamp);
    emplaceAbortReasonIfExists(donorCtx, abortReason);

    return DonorShardEntry{std::move(shardId), std::move(donorCtx)};
}

RecipientShardEntry makeRecipientShard(ShardId shardId,
                                       RecipientStateEnum recipientState,
                                       boost::optional<Status> abortReason) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(recipientState);
    emplaceAbortReasonIfExists(recipientCtx, abortReason);

    return RecipientShardEntry{std::move(shardId), std::move(recipientCtx)};
}

UUID getCollectionUUIDFromChunkManger(const NamespaceString& originalNss, const ChunkManager& cm) {
    auto collectionUUID = cm.getUUID();
    uassert(ErrorCodes::InvalidUUID,
            "Cannot reshard collection {} due to missing UUID"_format(originalNss.ns()),
            collectionUUID);

    return collectionUUID.get();
}

NamespaceString constructTemporaryReshardingNss(StringData db, const UUID& sourceUuid) {
    return NamespaceString(db,
                           fmt::format("{}{}",
                                       NamespaceString::kTemporaryReshardingCollectionPrefix,
                                       sourceUuid.toString()));
}

std::set<ShardId> getRecipientShards(OperationContext* opCtx,
                                     const NamespaceString& sourceNss,
                                     const UUID& reshardingUUID) {
    const auto& tempNss = constructTemporaryReshardingNss(sourceNss.db(), reshardingUUID);
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, tempNss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << tempNss << " to be sharded",
            cm.isSharded());

    std::set<ShardId> recipients;
    cm.getAllShardIds(&recipients);
    return recipients;
}

void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern) {
    std::sort(chunks.begin(), chunks.end(), [](const ReshardedChunk& a, const ReshardedChunk& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
    });
    // Check for global minKey and maxKey
    uassert(ErrorCodes::BadValue,
            "Chunk range must start at global min for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.front().getMin() ==
                                                        keyPattern.globalMin()));
    uassert(ErrorCodes::BadValue,
            "Chunk range must end at global max for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.back().getMax() ==
                                                        keyPattern.globalMax()));

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto chunk : chunks) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Chunk ranges must be contiguous",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() == chunk.getMin()));
        }
        prevMax = boost::optional<BSONObj>(chunk.getMax());
    }
}

void validateReshardedChunks(const std::vector<mongo::BSONObj>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern) {
    std::vector<ReshardedChunk> validChunks;
    for (const BSONObj& obj : chunks) {
        auto chunk = ReshardedChunk::parse(IDLParserErrorContext("reshardedChunks"), obj);
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getRecipientShardId()));
        validChunks.push_back(chunk);
    }
    checkForHolesAndOverlapsInChunks(validChunks, keyPattern);
}

Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards) {
    invariant(!donorShards.empty());

    auto maxMinFetchTimestamp = Timestamp::min();
    for (auto& donor : donorShards) {
        auto donorFetchTimestamp = donor.getMutableState().getMinFetchTimestamp();
        uassert(4957300,
                "All donors must have a minFetchTimestamp, but donor {} does not."_format(
                    StringData{donor.getId()}),
                donorFetchTimestamp.is_initialized());
        if (maxMinFetchTimestamp < donorFetchTimestamp.value()) {
            maxMinFetchTimestamp = donorFetchTimestamp.value();
        }
    }
    return maxMinFetchTimestamp;
}

void checkForOverlappingZones(std::vector<ReshardingZoneType>& zones) {
    std::sort(
        zones.begin(), zones.end(), [](const ReshardingZoneType& a, const ReshardingZoneType& b) {
            return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
        });

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto zone : zones) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Zone ranges must not overlap",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() <= zone.getMin()));
        }
        prevMax = boost::optional<BSONObj>(zone.getMax());
    }
}

std::vector<BSONObj> buildTagsDocsFromZones(const NamespaceString& tempNss,
                                            const std::vector<ReshardingZoneType>& zones) {
    std::vector<BSONObj> tags;
    tags.reserve(zones.size());
    for (const auto& zone : zones) {
        ChunkRange range(zone.getMin(), zone.getMax());
        TagsType tag(tempNss, zone.getZone().toString(), range);
        tags.push_back(tag.toBSON());
    }

    return tags;
}

void createSlimOplogView(OperationContext* opCtx, Database* db) {
    writeConflictRetry(
        opCtx, "createReshardingSlimOplog", NamespaceString::kReshardingOplogView.ns(), [&] {
            {
                // Create 'system.views' in a separate WUOW if it does not exist.
                WriteUnitOfWork wuow(opCtx);
                CollectionPtr coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                    opCtx, NamespaceString(db->getSystemViewsName()));
                if (!coll) {
                    coll = db->createCollection(opCtx, NamespaceString(db->getSystemViewsName()));
                }
                invariant(coll);
                wuow.commit();
            }

            // Resharding uses the `prevOpTime` to link oplog related entries via a
            // $graphLookup. Large transactions and prepared transaction use prevOpTime to identify
            // earlier oplog entries from the same transaction. Retryable writes (identified via the
            // presence of `stmtId`) use prevOpTime to identify earlier run statements from the same
            // retryable write.  This view will unlink oplog entries from the same retryable write
            // by zeroing out their `prevOpTime`.
            CollectionOptions options;
            options.viewOn = NamespaceString::kRsOplogNamespace.coll().toString();
            options.pipeline = BSON_ARRAY(getSlimOplogPipeline());
            WriteUnitOfWork wuow(opCtx);
            auto status = db->createView(opCtx, NamespaceString::kReshardingOplogView, options);
            if (status == ErrorCodes::NamespaceExists) {
                return;
            }
            uassertStatusOK(status);
            wuow.commit();
        });
}

BSONObj getSlimOplogPipeline() {
    return fromjson(
        "{$project: {\
            _id: '$ts',\
            op: 1,\
            o: {\
                applyOps: {ui: 1, destinedRecipient: 1},\
                abortTransaction: 1\
            },\
            ts: 1,\
            'prevOpTime.ts': {$cond: {\
                if: {$eq: [{$type: '$stmtId'}, 'missing']},\
                then: '$prevOpTime.ts',\
                else: Timestamp(0, 0)\
            }}\
        }}");
}

std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;
    const Value EXISTS = V{Doc{{"$exists", true}}};
    const Value DNE = V{Doc{{"$exists", false}}};

    Pipeline::SourceContainer stages;
    // The node receiving the query verifies continuity of oplog entries (i.e: that the recipient
    // hasn't fallen off the oplog). This stage provides the input timestamp that the donor uses for
    // verification.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"ts", Doc{{"$gte", startAfter.getTs()}}}}.toBson(), expCtx));

    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             // Only capture CRUD operations relevant for the `destinedRecipient`.
             Arr{V{Doc{{"op", Doc{{"$in", Arr{V{"i"_sd}, V{"u"_sd}, V{"d"_sd}, V{"n"_sd}}}}},
                       {"ui", collUUID},
                       {"destinedRecipient", recipientShard.toString()}}},
                 // Capture all commands. One cannot determine if a command is relevant to the
                 // `destinedRecipient` until after oplog chaining via `prevOpTime` is resolved.
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps", EXISTS},
                       {"o.partialTxn", DNE},
                       {"o.prepare", DNE}}},
                 V{Doc{{"op", "c"_sd}, {"o.commitTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"o.abortTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"ui", collUUID}}}}}}
            .toBson(),
        expCtx));

    // Denormalize oplog chaining. This will shove meta-information (particularly timestamps and
    // `destinedRecipient`) into the current aggregation output (still a raw oplog entry). This
    // meta-information is used for performing $lookups against the timestamp field and filtering
    // out earlier commands where the necessary `destinedRecipient` data wasn't yet available.
    stages.emplace_back(DocumentSourceGraphLookUp::create(
        expCtx,
        NamespaceString("local.system.resharding.slimOplogForGraphLookup"),  // from
        "history",                                                           // as
        "prevOpTime.ts",                                                     // connectFromField
        "ts",                                                                // connectToField
        ExpressionFieldPath::parse(expCtx.get(),
                                   "$ts",
                                   expCtx->variablesParseState),  // startWith
        boost::none,                                              // additionalFilter
        boost::optional<FieldPath>("depthForResharding"),         // depthField
        boost::none,                                              // maxDepth
        boost::none));                                            // unwindSrc

    // Only keep oplog entries for the relevant `destinedRecipient`.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             Arr{V{Doc{{"history", Doc{{"$size", 1}}},
                       {"$or",
                        Arr{V{Doc{{"history.0.op", Doc{{"$ne", "c"_sd}}}}},
                            V{Doc{{"history.0.op", "c"_sd}, {"history.0.o.applyOps", DNE}}}}}}},
                 V{Doc{{"history",
                        Doc{{"$elemMatch",
                             Doc{{"op", "c"_sd},
                                 {"o.applyOps",
                                  Doc{{"$elemMatch",
                                       Doc{{"ui", collUUID},
                                           {"destinedRecipient",
                                            recipientShard.toString()}}}}}}}}}}}}}}
            .toBson(),
        expCtx));

    // There's no guarantee to the order of entries accumulated in $graphLookup. The $reduce
    // expression sorts the `history` array in ascending `depthForResharding` order. The
    // $reverseArray expression will give an array in ascending timestamp order.
    stages.emplace_back(DocumentSourceAddFields::create(fromjson("{\
                    history: {$reverseArray: {$reduce: {\
                        input: '$history',\
                        initialValue: {$range: [0, {$size: '$history'}]},\
                        in: {$concatArrays: [\
                            {$slice: ['$$value', '$$this.depthForResharding']},\
                            ['$$this'],\
                            {$slice: [\
                                '$$value',\
                                {$subtract: [\
                                    {$add: ['$$this.depthForResharding', 1]},\
                                    {$size: '$history'}]}]}]}}}}}"),
                                                        expCtx));

    // If the last entry in the history is an `abortTransaction`, leave the `abortTransaction` oplog
    // entry in place, but remove all prior `applyOps` entries. The `abortTransaction` entry is
    // required to update the `config.transactions` table. Removing the `applyOps` entries ensures
    // we don't make any data writes that would have to be undone.
    stages.emplace_back(DocumentSourceAddFields::create(fromjson("{\
                        'history': {$let: {\
                            vars: {lastEntry: {$arrayElemAt: ['$history', -1]}},\
                            in: {$cond: {\
                                if: {$and: [\
                                    {$eq: ['$$lastEntry.op', 'c']},\
                                    {$ne: [{$type: '$$lastEntry.o.abortTransaction'}, 'missing']}\
                                ]},\
                                then: ['$$lastEntry'],\
                                else: '$history'}}}}}"),
                                                        expCtx));

    // Unwind the history array. The output at this point is a new stream of oplog entries, each
    // with exactly one history element. If there are no multi-oplog transactions (e.g: large
    // transactions, prepared transactions), the documents will be in timestamp order. In the
    // presence of large or prepared transactions, the data writes that were part of prior oplog
    // entries will be adjacent to each other, terminating with a `commitTransaction` oplog entry.
    stages.emplace_back(DocumentSourceUnwind::create(expCtx, "history", false, boost::none));

    // Group the relevant timestamps into an `_id` field. The `_id.clusterTime` value is the
    // timestamp of the last entry in a multi-oplog entry transaction. The `_id.ts` value is the
    // timestamp of the oplog entry that operation appeared in. For typical CRUD operations, these
    // are the same. In multi-oplog entry transactions, `_id.clusterTime` may be later than
    // `_id.ts`.
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceRoot: {newRoot: {$mergeObjects: [\
                     '$history',\
                     {_id: {clusterTime: '$ts', ts: '$history.ts'}}]}}}")
            .firstElement(),
        expCtx));

    // Now that the chained oplog entries are adjacent with an annotated `ReshardingDonorOplogId`,
    // the pipeline can prune anything earlier than the resume time.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"_id", Doc{{"$gt", startAfter.toBSON()}}}}.toBson(), expCtx));

    // Using the `ts` field, attach the full oplog document. Note that even for simple oplog
    // entries, the oplog contents were thrown away making this step necessary for all documents.
    stages.emplace_back(DocumentSourceLookUp::createFromBson(Doc{{"$lookup",
                                                                  Doc{{"from", "oplog.rs"_sd},
                                                                      {"localField", "ts"_sd},
                                                                      {"foreignField", "ts"_sd},
                                                                      {"as", "fullEntry"_sd}}}}
                                                                 .toBson()
                                                                 .firstElement(),
                                                             expCtx));

    // The outer fields of the pipeline document only contain meta-information about the
    // operation. The prior `$lookup` places the actual operations into a `fullEntry` array of size
    // one (timestamps are unique, thus always exactly one value).
    stages.emplace_back(DocumentSourceUnwind::create(expCtx, "fullEntry", false, boost::none));

    // Keep only the oplog entry from the `$lookup` merged with the `_id`.
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceRoot: {newRoot: {$mergeObjects: ['$fullEntry', {_id: '$_id'}]}}}")
            .firstElement(),
        expCtx));

    // Filter out anything inside of an `applyOps` specifically destined for another shard. This
    // ensures zone restrictions are obeyed. Data will never be sent to a shard that it isn't meant
    // to end up on.
    stages.emplace_back(DocumentSourceAddFields::create(
        Doc{{"o.applyOps",
             Doc{{"$cond",
                  Doc{{"if", Doc{{"$eq", Arr{V{"$op"_sd}, V{"c"_sd}}}}},
                      {"then",
                       Doc{{"$filter",
                            Doc{{"input", "$o.applyOps"_sd},
                                {"cond",
                                 Doc{{"$and",
                                      Arr{V{Doc{{"$eq", Arr{V{"$$this.ui"_sd}, V{collUUID}}}}},
                                          V{Doc{{"$eq",
                                                 Arr{V{"$$this.destinedRecipient"_sd},
                                                     V{recipientShard.toString()}}}}}}}}}}}}},
                      {"else", "$o.applyOps"_sd}}}}}}
            .toBson(),
        expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

boost::optional<ShardId> getDestinedRecipient(OperationContext* opCtx,
                                              const NamespaceString& sourceNss,
                                              const BSONObj& fullDocument,
                                              CollectionShardingState* css,
                                              const ScopedCollectionDescription& collDesc) {
    if (!ShardingState::get(opCtx)->enabled()) {
        // Don't bother looking up the sharding state for the collection if the server isn't even
        // running with sharding enabled. We know there couldn't possibly be any resharding fields.
        return boost::none;
    }

    auto reshardingKeyPattern = collDesc.getReshardingKeyIfShouldForwardOps();
    if (!reshardingKeyPattern)
        return boost::none;

    if (!documentBelongsToMe(opCtx, css, collDesc, fullDocument))
        return boost::none;

    bool allowLocks = true;
    auto tempNssRoutingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
            opCtx,
            constructTemporaryReshardingNss(sourceNss.db(), getCollectionUuid(opCtx, sourceNss)),
            allowLocks));

    auto shardKey = reshardingKeyPattern->extractShardKeyFromDocThrows(fullDocument);

    return tempNssRoutingInfo.findIntersectingChunkWithSimpleCollation(shardKey).getShardId();
}

bool isFinalOplog(const repl::OplogEntry& oplog) {
    if (oplog.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    auto o2Field = oplog.getObject2();
    if (!o2Field) {
        return false;
    }

    return o2Field->getField("type").valueStringDataSafe() == kReshardFinalOpLogType;
}

bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID) {
    if (!isFinalOplog(oplog)) {
        return false;
    }

    return uassertStatusOK(UUID::parse(oplog.getObject2()->getField("reshardingUUID"))) ==
        reshardingUUID;
}


NamespaceString getLocalOplogBufferNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString("config.localReshardingOplogBuffer.{}.{}"_format(
        existingUUID.toString(), donorShardId.toString()));
}

NamespaceString getLocalConflictStashNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString{NamespaceString::kConfigDb,
                           "localReshardingConflictStash.{}.{}"_format(existingUUID.toString(),
                                                                       donorShardId.toString())};
}
}  // namespace mongo

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
#pragma once

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/resharded_chunk_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_request.h"

namespace mongo {

constexpr auto kReshardingOplogPrePostImageOps = "prePostImageOps"_sd;

/**
 * Emplaces the 'fetchTimestamp' onto the ClassWithFetchTimestamp if the timestamp has been
 * emplaced inside the boost::optional.
 */
template <class ClassWithFetchTimestamp>
void emplaceFetchTimestampIfExists(ClassWithFetchTimestamp& c,
                                   boost::optional<Timestamp> fetchTimestamp) {
    if (!fetchTimestamp) {
        return;
    }

    invariant(!fetchTimestamp->isNull());

    if (auto alreadyExistingFetchTimestamp = c.getFetchTimestamp()) {
        invariant(fetchTimestamp == alreadyExistingFetchTimestamp);
    }

    FetchTimestamp fetchTimestampStruct;
    fetchTimestampStruct.setFetchTimestamp(std::move(fetchTimestamp));
    c.setFetchTimestampStruct(std::move(fetchTimestampStruct));
}

/**
 * Helper method to construct a DonorShardEntry with the fields specified.
 */
DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp = boost::none);

/**
 * Helper method to construct a RecipientShardEntry with the fields specified.
 */
RecipientShardEntry makeRecipientShard(
    ShardId shardId,
    RecipientStateEnum recipientState,
    boost::optional<Timestamp> strictConsistencyTimestamp = boost::none);

/**
 * Gets the UUID for 'nss' from the 'cm'
 *
 * Note: throws if the collection does not have a UUID.
 */
UUID getCollectionUUIDFromChunkManger(const NamespaceString& nss, const ChunkManager& cm);

/**
 * Assembles the namespace string for the temporary resharding collection based on the source
 * namespace components.
 *
 *      <db>.system.resharding.<existing collection's UUID>
 */
NamespaceString constructTemporaryReshardingNss(StringData db, const UUID& sourceUuid);

/**
 * Sends _flushRoutingTableCacheUpdatesWithWriteConcern to a list of shards. Throws if one of the
 * shards fails to refresh.
 */
void tellShardsToRefresh(OperationContext* opCtx,
                         const std::vector<ShardId>& shardIds,
                         const NamespaceString& nss,
                         std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Asserts that there is not a hole or overlap in the chunks.
 */
void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern);

/**
 * Validates resharded chunks provided with a reshardCollection cmd. Parses each BSONObj to a valid
 * ReshardedChunk and asserts that each chunk's shardId is associated with an existing entry in
 * the shardRegistry. Then, asserts that there is not a hole or overlap in the chunks.
 */
void validateReshardedChunks(const std::vector<mongo::BSONObj>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern);

/**
 * Selects the highest minFetchTimestamp from the list of donors.
 *
 * Throws if not every donor has a minFetchTimestamp.
 */
Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards);

/**
 * Asserts that there is not an overlap in the zone ranges.
 */
void checkForOverlappingZones(std::vector<TagsType>& zones);

/**
 * Validates zones provided with a reshardCollection cmd. Parses each BSONObj to a valid
 * TagsType and asserts that each zones's name is associated with an existing entry in
 * config.tags. Then, asserts that there is not an overlap in the zone ranges.
 */
void validateZones(const std::vector<mongo::BSONObj>& zones,
                   const std::vector<TagsType>& authoritativeTags);

/**
 * Create pipeline stages for iterating the buffered copy of the donor oplog and link together the
 * oplog entries with their preImage/postImage oplog. Note that caller is responsible for making
 * sure that the donorOplogNS is properly resolved and ns is set in the expCtx.
 *
 * If doAttachDocumentCursor is false, the caller will need to manually set the initial stage of the
 * pipeline with a source. This is mostly useful for testing.
 */
std::unique_ptr<Pipeline, PipelineDeleter> createAggForReshardingOplogBuffer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<ReshardingDonorOplogId>& resumeToken,
    bool doAttachDocumentCursor);

/**
 * Creates a view on the oplog that facilitates the specialized oplog tailing a resharding
 * recipient performs on a donor.
 */
void createSlimOplogView(OperationContext* opCtx, Database* db);

BSONObj getSlimOplogPipeline();

/**
 * Creates a pipeline that can be serialized into a query for fetching oplog entries. `startAfter`
 * may be `Timestamp::isNull()` to fetch from the beginning of the oplog.
 */
std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard,
    bool doesDonorOwnMinKeyChunk);

/**
 * Returns the shard Id of the recipient shard that would own the document under the new shard
 * key pattern.
 */
boost::optional<ShardId> getDestinedRecipient(OperationContext* opCtx,
                                              const NamespaceString& sourceNss,
                                              const BSONObj& fullDocument);

/**
 * Sentinel oplog format:
 * {
 *   op: "n",
 *   ns: "<database>.<collection>",
 *   ui: <existingUUID>,
 *   destinedRecipient: <recipientShardId>,
 *   o: {msg: "Writes to <database>.<collection> is temporarily blocked for resharding"},
 *   o2: {type: "reshardFinalOp", reshardingUUID: <reshardingUUID>},
 *   fromMigrate: true,
 * }
 */
bool isFinalOplog(const repl::OplogEntry& oplog);
bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID);

NamespaceString getLocalOplogBufferNamespace(UUID reshardingUUID, ShardId donorShardId);

}  // namespace mongo

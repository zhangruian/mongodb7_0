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

#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/future.h"

namespace mongo {

class CreateCollectionCoordinator final
    : public ShardingDDLCoordinator_NORESILIENT,
      public std::enable_shared_from_this<CreateCollectionCoordinator> {
public:
    CreateCollectionCoordinator(OperationContext* opCtx, const ShardsvrCreateCollection& request);

    /**
     * Returns the information of the newly created collection, or the already existing one. It must
     * be called after a successfull execution of run.
     */
    const CreateCollectionResponse& getResultOnSuccess() {
        return *_result;
    }

private:
    SemiFuture<void> runImpl(std::shared_ptr<executor::TaskExecutor> executor) override;

    /**
     * Performs all required checks before holding the critical sections.
     */
    void _checkCommandArguments(OperationContext* opCtx);

    /**
     * Ensures the collection is created locally and has the appropiate shard index.
     */
    void _createCollectionAndIndexes(OperationContext* opCtx);

    /**
     * Given the appropiate split policy, create the initial chunks.
     */
    void _createChunks(OperationContext* opCtx);

    /**
     * If the optimized path can be taken, ensure the collection is already created in all the
     * participant shards.
     */
    void _createCollectionOnNonPrimaryShards(OperationContext* opCtx);

    /**
     * Does the following writes:
     * 1. Updates the config.collections entry for the new sharded collection
     * 2. Updates config.chunks entries for the new sharded collection
     */
    void _commit(OperationContext* opCtx);

    /**
     * Refresh all participant shards and log creation.
     */
    void _cleanup(OperationContext* opCtx);

    ServiceContext* _serviceContext;
    const ShardsvrCreateCollection _request;
    const NamespaceString& _nss;

    boost::optional<ShardKeyPattern> _shardKeyPattern;
    boost::optional<BSONObj> _collation;
    boost::optional<UUID> _collectionUUID;
    std::unique_ptr<InitialSplitPolicy> _splitPolicy;
    InitialSplitPolicy::ShardCollectionConfig _initialChunks;
    boost::optional<CreateCollectionResponse> _result;
};

}  // namespace mongo

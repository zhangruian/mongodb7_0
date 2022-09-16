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


#include "mongo/db/s/create_collection_coordinator_document_gen.h"
#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/remove_chunks_gen.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

struct OptionsAndIndexes {
    BSONObj options;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
};

OptionsAndIndexes getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nssOrUUID) {
    DBDirectClient localClient(opCtx);
    BSONObj idIndex;
    BSONObjBuilder optionsBob;

    auto all =
        localClient.getCollectionInfos(nssOrUUID.dbname(), BSON("info.uuid" << *nssOrUUID.uuid()));

    // There must be a collection at this time.
    invariant(!all.empty());
    auto& entry = all.front();

    if (entry["options"].isABSONObj()) {
        optionsBob.appendElements(entry["options"].Obj());
    }
    optionsBob.append(entry["info"]["uuid"]);
    if (entry["idIndex"]) {
        idIndex = entry["idIndex"].Obj().getOwned();
    }

    auto indexSpecsList = localClient.getIndexSpecs(nssOrUUID, false, 0);

    return {optionsBob.obj(),
            std::vector<BSONObj>(std::begin(indexSpecsList), std::end(indexSpecsList)),
            idIndex};
}

// NOTES on the 'collation' optional parameter contained by the shardCollection() request:
// 1. It specifies the ordering criteria that will be applied when comparing chunk boundaries
// during sharding operations (such as move/mergeChunks).
// 2. As per today, the only supported value (and the one applied by default) is 'simple'
// collation.
// 3. If the collection being sharded does not exist yet, it will also be used as the ordering
// criteria to serve user queries over the shard index fields.
// 4. If an existing unsharded collection is being targeted, the original 'collation' will still
// be used to serve user queries, but the shardCollection is required to explicitly include the
// 'collation' parameter to succeed (as an acknowledge of what specified in points 1. and 2.)
BSONObj resolveCollationForUserQueries(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const boost::optional<BSONObj>& collationInRequest) {
    // Ensure the collation is valid. Currently we only allow the simple collation.
    std::unique_ptr<CollatorInterface> requestedCollator = nullptr;
    if (collationInRequest) {
        requestedCollator =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(collationInRequest.value()));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collationInRequest.value(),
                !requestedCollator);
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    const auto actualCollator = [&]() -> const CollatorInterface* {
        const auto& coll = autoColl.getCollection();
        if (coll) {
            uassert(
                ErrorCodes::InvalidOptions, "can't shard a capped collection", !coll->isCapped());
            return coll->getDefaultCollator();
        }

        return nullptr;
    }();

    if (!requestedCollator && !actualCollator)
        return BSONObj();

    auto actualCollation = actualCollator->getSpec();
    auto actualCollatorBSON = actualCollation.toBSON();

    if (!collationInRequest) {
        auto actualCollatorFilter =
            uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                ->makeFromBSON(actualCollatorBSON));
        uassert(ErrorCodes::BadValue,
                str::stream() << "If no collation was specified, the collection collation must be "
                                 "{locale: 'simple'}, "
                              << "but found: " << actualCollatorBSON,
                !actualCollatorFilter);
    }

    return actualCollatorBSON;
}

/**
 * Constructs the BSON specification document for the create collections command using the given
 * namespace, collation, and timeseries options.
 */
BSONObj makeCreateCommand(const NamespaceString& nss,
                          const boost::optional<Collation>& collation,
                          const TimeseriesOptions& tsOpts) {
    CreateCommand create(nss);
    create.setTimeseries(tsOpts);
    if (collation) {
        create.setCollation(*collation);
    }
    BSONObj commandPassthroughFields;
    return create.toBSON(commandPassthroughFields);
}

/**
 * Compares the proposed shard key with the shard key of the collection's existing zones
 * to ensure they are a legal combination.
 */
void validateShardKeyAgainstExistingZones(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& proposedKey,
                                          const std::vector<TagsType>& tags) {
    const AutoGetCollection coll(opCtx, nss, MODE_IS);
    for (const auto& tag : tags) {
        BSONObjIterator tagMinFields(tag.getMinKey());
        BSONObjIterator tagMaxFields(tag.getMaxKey());
        BSONObjIterator proposedFields(proposedKey);

        while (tagMinFields.more() && proposedFields.more()) {
            BSONElement tagMinKeyElement = tagMinFields.next();
            BSONElement tagMaxKeyElement = tagMaxFields.next();
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the min and max of the existing zone " << tag.getMinKey()
                                  << " -->> " << tag.getMaxKey() << " have non-matching keys",
                    tagMinKeyElement.fieldNameStringData() ==
                        tagMaxKeyElement.fieldNameStringData());

            BSONElement proposedKeyElement = proposedFields.next();
            bool match = ((tagMinKeyElement.fieldNameStringData() ==
                           proposedKeyElement.fieldNameStringData()) &&
                          ((tagMinFields.more() && proposedFields.more()) ||
                           (!tagMinFields.more() && !proposedFields.more())));
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "the proposed shard key " << proposedKey.toString()
                                  << " does not match with the shard key of the existing zone "
                                  << tag.getMinKey() << " -->> " << tag.getMaxKey(),
                    match);

            // If the field is hashed, make sure that the min and max values are of supported type.
            uassert(
                ErrorCodes::InvalidOptions,
                str::stream() << "cannot do hash sharding with the proposed key "
                              << proposedKey.toString() << " because there exists a zone "
                              << tag.getMinKey() << " -->> " << tag.getMaxKey()
                              << " whose boundaries are not of type NumberLong, MinKey or MaxKey",
                !ShardKeyPattern::isHashedPatternEl(proposedKeyElement) ||
                    (ShardKeyPattern::isValidHashedValue(tagMinKeyElement) &&
                     ShardKeyPattern::isValidHashedValue(tagMaxKeyElement)));

            if (coll && coll->getTimeseriesOptions()) {
                const std::string controlTimeField =
                    timeseries::kControlMinFieldNamePrefix.toString() +
                    coll->getTimeseriesOptions()->getTimeField();
                if (tagMinKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMinKeyElement.type() == MinKey);
                }
                if (tagMaxKeyElement.fieldNameStringData() == controlTimeField) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "time field cannot be specified in the zone range for "
                                             "time-series collections",
                            tagMaxKeyElement.type() == MinKey);
                }
            }
        }
    }
}

std::vector<TagsType> getTagsAndValidate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& proposedKey) {
    // Read zone info
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

    if (!tags.empty()) {
        validateShardKeyAgainstExistingZones(opCtx, nss, proposedKey, tags);
    }

    return tags;
}

bool checkIfCollectionIsEmpty(OperationContext* opCtx, const NamespaceString& nss) {
    // Use find with predicate instead of count in order to ensure that the count
    // command doesn't just consult the cached metadata, which may not always be
    // correct
    DBDirectClient localClient(opCtx);
    return localClient.findOne(nss, BSONObj{}).isEmpty();
}

int getNumShards(OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    shardRegistry->reload(opCtx);

    return shardRegistry->getNumShards(opCtx);
}

void cleanupPartialChunksFromPreviousAttempt(OperationContext* opCtx,
                                             const UUID& uuid,
                                             const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Remove the chunks matching uuid
    ConfigsvrRemoveChunks configsvrRemoveChunksCmd(uuid);
    configsvrRemoveChunksCmd.setDbName(NamespaceString::kAdminDb);

    const auto swRemoveChunksResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kAdminDb.toString(),
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveChunksCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swRemoveChunksResult)),
        str::stream() << "Error removing chunks matching uuid " << uuid);
}

void insertChunks(OperationContext* opCtx,
                  std::vector<ChunkType>& chunks,
                  const OperationSessionInfo& osi) {
    BatchedCommandRequest insertRequest([&]() {
        write_ops::InsertCommandRequest insertOp(ChunkType::ConfigNS);
        std::vector<BSONObj> entries;
        entries.reserve(chunks.size());
        for (const auto& chunk : chunks) {
            entries.push_back(chunk.toConfigBSON());
        }
        insertOp.setDocuments(entries);
        return insertOp;
    }());

    insertRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());
    {
        auto newClient =
            opCtx->getServiceContext()->makeClient("CreateCollectionCoordinator::insertChunks");
        {
            stdx::lock_guard<Client> lk(*newClient.get());
            newClient->setSystemOperationKillableByStepdown(lk);
        }

        AlternativeClientRegion acr(newClient);
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        auto newOpCtx = CancelableOperationContext(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);
        newOpCtx->setLogicalSessionId(*osi.getSessionId());
        newOpCtx->setTxnNumber(*osi.getTxnNumber());

        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        cluster::write(newOpCtx.get(), insertRequest, &stats, &response);
        uassertStatusOK(response.toStatus());
    }
}

void insertCollectionEntry(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionType& coll,
                           const OperationSessionInfo& osi) {
    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    BatchedCommandRequest insertRequest(
        write_ops::InsertCommandRequest(CollectionType::ConfigNS, {coll.toBSON()}));
    insertRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    const BSONObj cmdObj = insertRequest.toBSON().addFields(osi.toBSON());

    BatchedCommandResponse unusedResponse;
    uassertStatusOK(Shard::CommandResponse::processBatchWriteResponse(
        configShard->runCommand(opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                CollectionType::ConfigNS.db().toString(),
                                cmdObj,
                                Shard::kDefaultConfigCommandTimeout,
                                Shard::RetryPolicy::kIdempotent),
        &unusedResponse));
}

void broadcastDropCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const std::shared_ptr<executor::TaskExecutor>& executor,
                             const OperationSessionInfo& osi) {
    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    const ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss, participants, executor, osi);
}

}  // namespace

void CreateCollectionCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

const NamespaceString& CreateCollectionCoordinator::nss() const {
    // Rely on the resolved request parameters to retrieve the nss to be targeted by the
    // coordinator.
    stdx::lock_guard lk{_docMutex};
    invariant(_doc.getTranslatedRequestParams());
    return _doc.getTranslatedRequestParams()->getNss();
}

void CreateCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    // If we have two shard collections on the same namespace, then the arguments must be the same.
    const auto otherDoc = CreateCollectionCoordinatorDocument::parse(
        IDLParserContext("CreateCollectionCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another create collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _request.toBSON() == otherDoc.getCreateCollectionRequest().toBSON()));
}

ExecutorFuture<void> CreateCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kTranslateRequest) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _checkCommandArguments(opCtx);
                // Perform a preliminary check on whether the request may resolve into a no-op
                // before acquiring any critical section.
                auto createCollectionResponseOpt =
                    _checkIfCollectionAlreadyShardedWithSameOptions(opCtx);
                if (createCollectionResponseOpt) {
                    _result = createCollectionResponseOpt;
                    // Launch an exception to directly jump to the end of the continuation chain
                    uasserted(ErrorCodes::RequestAlreadyFulfilled,
                              str::stream() << "The collection" << originalNss()
                                            << "was already sharded by a past request");
                }
            }
        })
        .then([this, anchor = shared_from_this()] {
            if (_timeseriesNssResolvedByCommandHandler()) {
                return;
            }
            _executePhase(Phase::kTranslateRequest, [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                _logStartCreateCollection(opCtx);

                // Enter the critical sections before patching the user request to
                // avoid data races with concurrenct creation of unsharded
                // collections referencing the same namespace(s).
                _acquireCriticalSections(opCtx);
                _doc.setTranslatedRequestParams(_translateRequestParameters(opCtx));
            })();
        })
        .then(_executePhase(
            Phase::kCommit,
            [this, executor = executor, token, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    //
                    // Additionally we want to perform a majority write on the CSRS to ensure that
                    // all the subsequent reads will see all the writes performed from a previous
                    // execution of this coordinator.
                    _updateSession(opCtx);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(), **executor);
                }

                if (_timeseriesNssResolvedByCommandHandler()) {
                    // execute the logic of the kTranslateRequest phase now.
                    _logStartCreateCollection(opCtx);
                    _acquireCriticalSections(opCtx);
                    _doc.setTranslatedRequestParams(_translateRequestParameters(opCtx));
                }

                // Check if the collection was already sharded by a past request
                if (auto createCollectionResponseOpt =
                        sharding_ddl_util::checkIfCollectionAlreadySharded(
                            opCtx,
                            nss(),
                            _doc.getTranslatedRequestParams()->getKeyPattern().toBSON(),
                            _doc.getTranslatedRequestParams()->getCollation(),
                            _request.getUnique().value_or(false))) {

                    // A previous request already created and committed the collection but there was
                    // a stepdown after the commit.
                    _releaseCriticalSections(opCtx);

                    _result = createCollectionResponseOpt;
                    return;
                }

                if (!_firstExecution) {
                    auto uuid = sharding_ddl_util::getCollectionUUID(opCtx, nss());
                    // If the collection can be found locally, then we clean up the config.chunks
                    // collection.
                    if (uuid) {
                        LOGV2_DEBUG(5458704,
                                    1,
                                    "Removing partial changes from previous run",
                                    "namespace"_attr = nss());

                        _updateSession(opCtx);
                        cleanupPartialChunksFromPreviousAttempt(opCtx, *uuid, getCurrentSession());

                        _updateSession(opCtx);
                        broadcastDropCollection(opCtx, nss(), **executor, getCurrentSession());
                    }
                }

                ShardKeyPattern shardKeyPattern(_doc.getTranslatedRequestParams()->getKeyPattern());
                _createPolicy(opCtx, shardKeyPattern);
                _createCollectionAndIndexes(opCtx, shardKeyPattern);

                audit::logShardCollection(opCtx->getClient(),
                                          nss().toString(),
                                          *_request.getShardKey(),
                                          _request.getUnique().value_or(false));

                if (_splitPolicy->isOptimized()) {
                    _createChunks(opCtx, shardKeyPattern);

                    // Block reads/writes from here on if we need to create the collection on other
                    // shards, this way we prevent reads/writes that should be redirected to another
                    // shard
                    _promoteCriticalSectionsToBlockReads(opCtx);

                    _updateSession(opCtx);
                    _createCollectionOnNonPrimaryShards(opCtx, getCurrentSession());

                    _commit(opCtx);
                }

                // End of the critical section, from now on, read and writes are permitted.
                _releaseCriticalSections(opCtx);

                // Slow path. Create chunks (which might incur in an index scan) and commit must be
                // done outside of the critical section to prevent writes from stalling in unsharded
                // collections.
                if (!_splitPolicy->isOptimized()) {
                    _createChunks(opCtx, shardKeyPattern);
                    _commit(opCtx);
                }
            }))
        .then([this] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);
            _logEndCreateCollection(opCtx);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5458702,
                            "Error running create collection",
                            "namespace"_attr = originalNss(),
                            "error"_attr = redact(status));

                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                _releaseCriticalSections(opCtx);
            }

            return status;
        });
}

boost::optional<CreateCollectionResponse>
CreateCollectionCoordinator::_checkIfCollectionAlreadyShardedWithSameOptions(
    OperationContext* opCtx) {
    // Perfom check in the translation phase if the request is coming from a C2C command; this will
    // allow to honor the contract with mongosync (see SERVER-67885 for details)
    if (_request.getCollectionUUID()) {
        return boost::none;
    }

    // Preliminary check is unsupported for DDL requests received by nodes running old FCVs.
    if (_timeseriesNssResolvedByCommandHandler()) {
        return boost::none;
    }

    // Check is there is a standard sharded collection that matches the original request parameters
    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
            opCtx, originalNss()));
    if (routingInfo.isSharded()) {
        auto requestMatchesExistingCollection = [&] {
            // No timeseries fields in request
            if (_request.getTimeseries()) {
                return false;
            }

            if (_request.getUnique().value_or(false) != routingInfo.isUnique()) {
                return false;
            }

            if (SimpleBSONObjComparator::kInstance.evaluate(
                    *_request.getShardKey() != routingInfo.getShardKeyPattern().toBSON())) {
                return false;
            }

            auto defaultCollator = routingInfo.getDefaultCollator()
                ? routingInfo.getDefaultCollator()->getSpec().toBSON()
                : BSONObj();
            if (SimpleBSONObjComparator::kInstance.evaluate(
                    defaultCollator !=
                    resolveCollationForUserQueries(
                        opCtx, originalNss(), _request.getCollation()))) {
                return false;
            }

            return true;
        }();

        uassert(ErrorCodes::AlreadyInitialized,
                str::stream() << "sharding already enabled for collection " << originalNss(),
                requestMatchesExistingCollection);

        CreateCollectionResponse response(
            {routingInfo.getVersion(), CollectionIndexes(routingInfo.getVersion(), boost::none)});
        response.setCollectionUUID(routingInfo.getUUID());
        return response;
    }

    // If the request is still unresolved, check if there is an existing TS buckets namespace that
    // may be matched by the request.
    auto bucketsNss = originalNss().makeTimeseriesBucketsNamespace();
    routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, bucketsNss));
    if (!routingInfo.isSharded()) {
        return boost::none;
    }

    auto requestMatchesExistingCollection = [&] {
        if (routingInfo.isUnique() != _request.getUnique().value_or(false)) {
            return false;
        }

        // Timeseries options match
        const auto& timeseriesOptionsOnDisk =
            (*routingInfo.getTimeseriesFields()).getTimeseriesOptions();
        if (_request.getTimeseries() &&
            !timeseries::optionsAreEqual(*_request.getTimeseries(), timeseriesOptionsOnDisk)) {
            return false;
        }

        auto defaultCollator = routingInfo.getDefaultCollator()
            ? routingInfo.getDefaultCollator()->getSpec().toBSON()
            : BSONObj();
        if (SimpleBSONObjComparator::kInstance.evaluate(
                defaultCollator !=
                resolveCollationForUserQueries(opCtx, bucketsNss, _request.getCollation()))) {
            return false;
        }

        // Same Key Pattern
        const auto& timeseriesOptions =
            _request.getTimeseries() ? *_request.getTimeseries() : timeseriesOptionsOnDisk;
        auto requestKeyPattern =
            uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
                timeseriesOptions, *_request.getShardKey()));
        if (SimpleBSONObjComparator::kInstance.evaluate(routingInfo.getShardKeyPattern().toBSON() !=
                                                        requestKeyPattern)) {
            return false;
        }
        return true;
    }();

    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << bucketsNss,
            requestMatchesExistingCollection);

    CreateCollectionResponse response(
        {routingInfo.getVersion(), CollectionIndexes(routingInfo.getVersion(), boost::none)});
    response.setCollectionUUID(routingInfo.getUUID());
    return response;
}

void CreateCollectionCoordinator::_checkCommandArguments(OperationContext* opCtx) {
    LOGV2_DEBUG(
        5277902, 2, "Create collection _checkCommandArguments", "namespace"_attr = originalNss());

    if (originalNss().db() == NamespaceString::kConfigDb) {
        // Only allowlisted collections in config may be sharded (unless we are in test mode)
        uassert(ErrorCodes::IllegalOperation,
                "only special collections in the config db may be sharded",
                originalNss() == NamespaceString::kLogicalSessionsNamespace);
    }

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !ShardKeyPattern(*_request.getShardKey()).isHashedPattern() ||
                !_request.getUnique().value_or(false));

    if (_timeseriesNssResolvedByCommandHandler()) {
        // Ensure that a time-series collection cannot be sharded unless the feature flag is
        // enabled.
        if (originalNss().isTimeseriesBucketsCollection()) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "can't shard time-series collection " << nss(),
                    feature_flags::gFeatureFlagShardedTimeSeries.isEnabled(
                        serverGlobalParams.featureCompatibility) ||
                        !timeseries::getTimeseriesOptions(opCtx, nss(), false));
        }
    }

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !originalNss().isSystem() ||
                originalNss() == NamespaceString::kLogicalSessionsNamespace ||
                originalNss().isTemporaryReshardingCollection() ||
                originalNss().isTimeseriesBucketsCollection());

    if (_request.getNumInitialChunks()) {
        // Ensure numInitialChunks is within valid bounds.
        // Cannot have more than kMaxSplitPoints initial chunks per shard. Setting a maximum of
        // 1,000,000 chunks in total to limit the amount of memory this command consumes so
        // there is less danger of an OOM error.

        const int maxNumInitialChunksForShards =
            Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx) * shardutil::kMaxSplitPoints;
        const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
        int numChunks = _request.getNumInitialChunks().value();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks cannot be more than either: "
                              << maxNumInitialChunksForShards << ", " << shardutil::kMaxSplitPoints
                              << " * number of shards; or " << maxNumInitialChunksTotal,
                numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                    numChunks <= maxNumInitialChunksTotal);
    }

    if (originalNss().db() == NamespaceString::kConfigDb) {
        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto findReponse = uassertStatusOK(
            configShard->exhaustiveFindOnConfig(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                repl::ReadConcernLevel::kMajorityReadConcern,
                                                originalNss(),
                                                BSONObj(),
                                                BSONObj(),
                                                1));

        auto numDocs = findReponse.docs.size();

        // If this is a collection on the config db, it must be empty to be sharded.
        uassert(ErrorCodes::IllegalOperation,
                "collections in the config db must be empty to be sharded",
                numDocs == 0);
    }
}

TranslatedRequestParams CreateCollectionCoordinator::_translateRequestParameters(
    OperationContext* opCtx) {
    auto performCheckOnCollectionUUID = [this, opCtx](const NamespaceString& resolvedNss) {
        AutoGetCollection coll{opCtx, resolvedNss, MODE_IS};
        checkCollectionUUIDMismatch(
            opCtx, resolvedNss, coll.getCollection(), _request.getCollectionUUID());
    };

    auto bucketsNs = originalNss().makeTimeseriesBucketsNamespace();
    auto existingBucketsColl =
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(opCtx, bucketsNs);

    auto targetingStandardCollection = !_request.getTimeseries() && !existingBucketsColl;

    if (_timeseriesNssResolvedByCommandHandler() || targetingStandardCollection) {
        const auto& resolvedNamespace = originalNss();
        performCheckOnCollectionUUID(resolvedNamespace);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace too long. Namespace: " << resolvedNamespace
                              << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);
        return TranslatedRequestParams(
            resolvedNamespace,
            *_request.getShardKey(),
            resolveCollationForUserQueries(opCtx, resolvedNamespace, _request.getCollation()));
    }

    // The request is targeting a new or existing Timeseries collection and the request has not been
    // patched yet.
    const auto& resolvedNamespace = bucketsNs;
    performCheckOnCollectionUUID(resolvedNamespace);
    uassert(ErrorCodes::IllegalOperation,
            "Sharding a timeseries collection feature is not enabled",
            feature_flags::gFeatureFlagShardedTimeSeries.isEnabled(
                serverGlobalParams.featureCompatibility));

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Namespace too long. Namespace: " << resolvedNamespace
                          << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
            resolvedNamespace.size() <= NamespaceString::MaxNsShardedCollectionLen);

    // Consolidate the related request parameters...
    auto existingTimeseriesOptions = [&bucketsNs, &existingBucketsColl] {
        if (!existingBucketsColl) {
            return boost::optional<TimeseriesOptions>();
        }

        uassert(6159000,
                str::stream() << "the collection '" << bucketsNs
                              << "' does not have 'timeseries' options",
                existingBucketsColl->getTimeseriesOptions());
        return existingBucketsColl->getTimeseriesOptions();
    }();

    if (_request.getTimeseries() && existingTimeseriesOptions) {
        uassert(5731500,
                str::stream() << "the 'timeseries' spec provided must match that of exists '"
                              << originalNss() << "' collection",
                timeseries::optionsAreEqual(*_request.getTimeseries(), *existingTimeseriesOptions));
    } else if (!_request.getTimeseries()) {
        _request.setTimeseries(existingTimeseriesOptions);
    }

    // check that they are consistent with the requested shard key before creating the key pattern
    // object.
    auto timeFieldName = _request.getTimeseries()->getTimeField();
    auto metaFieldName = _request.getTimeseries()->getMetaField();
    BSONObjIterator shardKeyElems{*_request.getShardKey()};
    while (auto elem = shardKeyElems.next()) {
        if (elem.fieldNameStringData() == timeFieldName) {
            uassert(5914000,
                    str::stream() << "the time field '" << timeFieldName
                                  << "' can be only at the end of the shard key pattern",
                    !shardKeyElems.more());
        } else {
            uassert(5914001,
                    str::stream() << "only the time field or meta field can be "
                                     "part of shard key pattern",
                    metaFieldName &&
                        (elem.fieldNameStringData() == *metaFieldName ||
                         elem.fieldNameStringData().startsWith(*metaFieldName + ".")));
        }
    }
    KeyPattern keyPattern(
        uassertStatusOK(timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(
            *_request.getTimeseries(), *_request.getShardKey())));
    return TranslatedRequestParams(
        resolvedNamespace,
        keyPattern,
        resolveCollationForUserQueries(opCtx, resolvedNamespace, _request.getCollation()));
}

bool CreateCollectionCoordinator::_timeseriesNssResolvedByCommandHandler() const {
    return operationType() == DDLCoordinatorTypeEnum::kCreateCollectionPre61Compatible;
}

void CreateCollectionCoordinator::_acquireCriticalSections(OperationContext* opCtx) {
    // TODO SERVER-68084 call ShardingRecoveryService without the try/catch block
    try {
        ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
            opCtx,
            originalNss(),
            _critSecReason,
            ShardingCatalogClient::kMajorityWriteConcern,
            boost::none);
    } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
        if (_timeseriesNssResolvedByCommandHandler()) {
            throw;
        }

        // In case we acquisition was rejected because it targets an existing view, the critical
        // section is not needed and the error can be dropped because:
        //   1. We will not shard the view namespace
        //   2. This collection will remain a view since we are holding the DDL coll lock and
        //   thus the collection can't be dropped.
        _doc.setDisregardCriticalSectionOnOriginalNss(true);
    }

    if (!_timeseriesNssResolvedByCommandHandler()) {
        // Preventively acquire the critical section protecting the buckets namespace that the
        // creation of a timeseries collection would require.
        const auto bucketsNamespace = originalNss().makeTimeseriesBucketsNamespace();
        ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
            opCtx, bucketsNamespace, _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
    }
}

void CreateCollectionCoordinator::_promoteCriticalSectionsToBlockReads(
    OperationContext* opCtx) const {
    // TODO SERVER-68084 call ShardingRecoveryService without the if blocks.
    if (!_doc.getDisregardCriticalSectionOnOriginalNss()) {
        ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
            opCtx, originalNss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
    }

    if (!_timeseriesNssResolvedByCommandHandler()) {
        const auto bucketsNamespace = originalNss().makeTimeseriesBucketsNamespace();
        ShardingRecoveryService::get(opCtx)->promoteRecoverableCriticalSectionToBlockAlsoReads(
            opCtx, bucketsNamespace, _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
    }
}

void CreateCollectionCoordinator::_releaseCriticalSections(OperationContext* opCtx) {
    // TODO SERVER-68084 call ShardingRecoveryService without the try/catch block.
    try {
        ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
            opCtx, originalNss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
    } catch (ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
        // Ignore the error (when it is raised, we can assume that no critical section for the view
        // was previously acquired).
    }

    if (!_timeseriesNssResolvedByCommandHandler()) {
        const auto bucketsNamespace = originalNss().makeTimeseriesBucketsNamespace();
        ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
            opCtx, bucketsNamespace, _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
    }
}

void CreateCollectionCoordinator::_createCollectionAndIndexes(
    OperationContext* opCtx, const ShardKeyPattern& shardKeyPattern) {
    LOGV2_DEBUG(
        5277903, 2, "Create collection _createCollectionAndIndexes", "namespace"_attr = nss());

    const auto& collationBSON = _doc.getTranslatedRequestParams()->getCollation();
    boost::optional<Collation> collation;
    if (!collationBSON.isEmpty()) {
        collation.emplace(
            Collation::parse(IDLParserContext("CreateCollectionCoordinator"), collationBSON));
    }

    // We need to implicitly create a timeseries view and underlying bucket collection.
    if (_collectionEmpty && _request.getTimeseries()) {
        // TODO SERVER-68084 Remove viewLock and the whole if section that constructs it while
        // releasing the critical section on the originalNss.
        boost::optional<AutoGetCollection> viewLock;
        if (auto criticalSectionAcquiredOnOriginalNss =
                !_doc.getDisregardCriticalSectionOnOriginalNss();
            !_timeseriesNssResolvedByCommandHandler() && criticalSectionAcquiredOnOriginalNss) {
            // This is the subcase of a not yet existing pair of view (originalNss)+ bucket (nss)
            // timeseries collection that the DDL will have to create. Due to the current
            // constraints of the code:
            // - Such creation cannot be performed while holding the critical section over the views
            // namespace (once the view gets created, the CS will not be releasable); instead,
            // exclusive access must be enforced through a collection lock
            // - The critical section cannot be released while holding a collection lock, so this
            // operation must be performed first (leaving a small window open to data races)
            ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                opCtx, originalNss(), _critSecReason, ShardingCatalogClient::kMajorityWriteConcern);
            _doc.setDisregardCriticalSectionOnOriginalNss(true);
            viewLock.emplace(opCtx,
                             originalNss(),
                             LockMode::MODE_X,
                             AutoGetCollection::Options{}.viewMode(
                                 auto_get_collection::ViewMode::kViewsPermitted));
            // Once the exclusive access has been reacquired, ensure that no data race occurred.
            auto catalog = CollectionCatalog::get(opCtx);
            if (catalog->lookupView(opCtx, originalNss()) ||
                catalog->lookupCollectionByNamespace(opCtx, originalNss())) {
                _completeOnError = true;
                uasserted(ErrorCodes::NamespaceExists,
                          str::stream() << "A conflicting DDL operation was completed while trying "
                                           "to shard collection: "
                                        << originalNss());
            }
        }

        const auto viewName = nss().getTimeseriesViewNamespace();
        auto createCmd = makeCreateCommand(viewName, collation, *_request.getTimeseries());

        BSONObj createRes;
        DBDirectClient localClient(opCtx);
        localClient.runCommand(nss().db().toString(), createCmd, createRes);
        auto createStatus = getStatusFromCommandResult(createRes);

        if (!createStatus.isOK() && createStatus.code() == ErrorCodes::NamespaceExists) {
            LOGV2_DEBUG(5909400,
                        3,
                        "Timeseries namespace already exists",
                        "namespace"_attr = viewName.toString());
        } else {
            uassertStatusOK(createStatus);
        }
    }

    shardkeyutil::validateShardKeyIsNotEncrypted(opCtx, nss(), shardKeyPattern);

    auto indexCreated = false;
    if (_request.getImplicitlyCreateIndex().value_or(true)) {
        indexCreated = shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
            opCtx,
            nss(),
            shardKeyPattern,
            collationBSON,
            _request.getUnique().value_or(false),
            _request.getEnforceUniquenessCheck().value_or(true),
            shardkeyutil::ValidationBehaviorsShardCollection(opCtx));
    } else {
        uassert(6373200,
                "Must have an index compatible with the proposed shard key",
                validShardKeyIndexExists(opCtx,
                                         nss(),
                                         shardKeyPattern,
                                         collationBSON,
                                         _request.getUnique().value_or(false) &&
                                             _request.getEnforceUniquenessCheck().value_or(true),
                                         shardkeyutil::ValidationBehaviorsShardCollection(opCtx)));
    }

    auto replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());

    if (!indexCreated) {
        replClientInfo.setLastOpToSystemLastOpTime(opCtx);
    }
    // Wait until the index is majority written, to prevent having the collection commited to
    // the config server, but the index creation rolled backed on stepdowns.
    WriteConcernResult ignoreResult;
    uassertStatusOK(waitForWriteConcern(opCtx,
                                        replClientInfo.getLastOp(),
                                        ShardingCatalogClient::kMajorityWriteConcern,
                                        &ignoreResult));

    _collectionUUID = *sharding_ddl_util::getCollectionUUID(opCtx, nss());
}

void CreateCollectionCoordinator::_createPolicy(OperationContext* opCtx,
                                                const ShardKeyPattern& shardKeyPattern) {
    LOGV2_DEBUG(6042001, 2, "Create collection _createPolicy", "namespace"_attr = nss());
    _collectionEmpty = checkIfCollectionIsEmpty(opCtx, nss());

    _splitPolicy = InitialSplitPolicy::calculateOptimizationStrategy(
        opCtx,
        shardKeyPattern,
        _request.getNumInitialChunks() ? *_request.getNumInitialChunks() : 0,
        _request.getPresplitHashedZones() ? *_request.getPresplitHashedZones() : false,
        _request.getInitialSplitPoints(),
        getTagsAndValidate(opCtx, nss(), shardKeyPattern.toBSON()),
        getNumShards(opCtx),
        *_collectionEmpty,
        !feature_flags::gNoMoreAutoSplitter.isEnabled(serverGlobalParams.featureCompatibility));
}

void CreateCollectionCoordinator::_createChunks(OperationContext* opCtx,
                                                const ShardKeyPattern& shardKeyPattern) {
    LOGV2_DEBUG(5277904, 2, "Create collection _createChunks", "namespace"_attr = nss());
    _initialChunks = _splitPolicy->createFirstChunks(
        opCtx, shardKeyPattern, {*_collectionUUID, ShardingState::get(opCtx)->shardId()});

    // There must be at least one chunk.
    invariant(_initialChunks);
    invariant(!_initialChunks->chunks.empty());
}

void CreateCollectionCoordinator::_createCollectionOnNonPrimaryShards(
    OperationContext* opCtx, const OperationSessionInfo& osi) {
    LOGV2_DEBUG(5277905,
                2,
                "Create collection _createCollectionOnNonPrimaryShards",
                "namespace"_attr = nss());

    std::vector<AsyncRequestsSender::Request> requests;
    std::set<ShardId> initializedShards;
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    NamespaceStringOrUUID nssOrUUID{nss().db().toString(), *_collectionUUID};
    auto [collOptions, indexes, idIndex] = getCollectionOptionsAndIndexes(opCtx, nssOrUUID);

    for (const auto& chunk : _initialChunks->chunks) {
        const auto& chunkShardId = chunk.getShard();
        if (chunkShardId == dbPrimaryShardId ||
            initializedShards.find(chunkShardId) != initializedShards.end()) {
            continue;
        }

        ShardsvrCreateCollectionParticipant createCollectionParticipantRequest(nss());
        createCollectionParticipantRequest.setCollectionUUID(*_collectionUUID);

        createCollectionParticipantRequest.setOptions(collOptions);
        createCollectionParticipantRequest.setIdIndex(idIndex);
        createCollectionParticipantRequest.setIndexes(indexes);

        requests.emplace_back(chunkShardId,
                              CommandHelpers::appendMajorityWriteConcern(
                                  createCollectionParticipantRequest.toBSON(osi.toBSON())));

        initializedShards.emplace(chunkShardId);
    }

    if (!requests.empty()) {
        auto responses = gatherResponses(opCtx,
                                         nss().db(),
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent,
                                         requests);

        // If any shards fail to create the collection, fail the entire shardCollection command
        // (potentially leaving incomplely created sharded collection)
        for (const auto& response : responses) {
            auto shardResponse = uassertStatusOKWithContext(
                std::move(response.swResponse),
                str::stream() << "Unable to create collection " << nss().ns() << " on "
                              << response.shardId);
            auto status = getStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(status.withContext(str::stream()
                                               << "Unable to create collection " << nss().ns()
                                               << " on " << response.shardId));

            auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
            uassertStatusOK(wcStatus.withContext(str::stream()
                                                 << "Unable to create collection " << nss().ns()
                                                 << " on " << response.shardId));
        }
    }
}

void CreateCollectionCoordinator::_commit(OperationContext* opCtx) {
    LOGV2_DEBUG(5277906, 2, "Create collection _commit", "namespace"_attr = nss());

    // Upsert Chunks.
    _updateSession(opCtx);
    insertChunks(opCtx, _initialChunks->chunks, getCurrentSession());

    CollectionType coll(nss(),
                        _initialChunks->collVersion().epoch(),
                        _initialChunks->collVersion().getTimestamp(),
                        Date_t::now(),
                        *_collectionUUID,
                        _doc.getTranslatedRequestParams()->getKeyPattern());

    if (_request.getTimeseries()) {
        TypeCollectionTimeseriesFields timeseriesFields;
        timeseriesFields.setTimeseriesOptions(*_request.getTimeseries());
        coll.setTimeseriesFields(std::move(timeseriesFields));
    }

    if (auto collationBSON = _doc.getTranslatedRequestParams()->getCollation();
        !collationBSON.isEmpty()) {
        coll.setDefaultCollation(collationBSON);
    }

    if (_request.getUnique()) {
        coll.setUnique(*_request.getUnique());
    }

    _updateSession(opCtx);
    try {
        insertCollectionEntry(opCtx, nss(), coll, getCurrentSession());

        notifyChangeStreamsOnShardCollection(opCtx, nss(), *_collectionUUID, _request.toBSON());

        LOGV2_DEBUG(5277907, 2, "Collection successfully committed", "namespace"_attr = nss());

        forceShardFilteringMetadataRefresh(opCtx, nss());
    } catch (const DBException& ex) {
        LOGV2(5277908,
              "Failed to obtain collection's shard version, so it will be recovered",
              "namespace"_attr = nss(),
              "error"_attr = redact(ex));

        // If the refresh fails, then set the shard version to UNKNOWN and let a future
        // operation to refresh the metadata.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetCollection autoColl(opCtx, nss(), MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss())->clearFilteringMetadata(opCtx);

        throw;
    }

    // Best effort refresh to warm up cache of all involved shards so we can have a cluster
    // ready to receive operations.
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto dbPrimaryShardId = ShardingState::get(opCtx)->shardId();

    std::set<ShardId> shardsRefreshed;
    for (const auto& chunk : _initialChunks->chunks) {
        const auto& chunkShardId = chunk.getShard();

        if (chunkShardId == dbPrimaryShardId ||
            shardsRefreshed.find(chunkShardId) != shardsRefreshed.end()) {
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, chunkShardId));
        shard->runFireAndForgetCommand(opCtx,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       NamespaceString::kAdminDb.toString(),
                                       BSON("_flushRoutingTableCacheUpdates" << nss().ns()));

        shardsRefreshed.emplace(chunkShardId);
    }

    LOGV2(5277901,
          "Created initial chunk(s)",
          "namespace"_attr = nss(),
          "numInitialChunks"_attr = _initialChunks->chunks.size(),
          "initialCollectionVersion"_attr = _initialChunks->collVersion());

    const auto placementVersion = _initialChunks->chunks.back().getVersion();
    auto result = CreateCollectionResponse(
        {placementVersion, CollectionIndexes(placementVersion, boost::none)});
    result.setCollectionUUID(_collectionUUID);
    _result = std::move(result);

    LOGV2(5458701,
          "Collection created",
          "namespace"_attr = nss(),
          "UUID"_attr = _result->getCollectionUUID(),
          "version"_attr = _result->getCollectionVersion());
}

void CreateCollectionCoordinator::_logStartCreateCollection(OperationContext* opCtx) {
    BSONObjBuilder collectionDetail;
    collectionDetail.append("shardKey", *_request.getShardKey());
    collectionDetail.append("collection", originalNss().ns());
    collectionDetail.append("primary", ShardingState::get(opCtx)->shardId().toString());
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.start", originalNss().ns(), collectionDetail.obj());
}

void CreateCollectionCoordinator::_logEndCreateCollection(OperationContext* opCtx) {
    BSONObjBuilder collectionDetail;
    _result->getCollectionUUID()->appendToBuilder(&collectionDetail, "uuid");
    collectionDetail.append("version", _result->getCollectionVersion().toString());
    if (_collectionEmpty)
        collectionDetail.append("empty", *_collectionEmpty);
    if (_initialChunks)
        collectionDetail.appendNumber("numChunks",
                                      static_cast<long long>(_initialChunks->chunks.size()));
    ShardingLogging::get(opCtx)->logChange(
        opCtx, "shardCollection.end", originalNss().ns(), collectionDetail.obj());
}

}  // namespace mongo

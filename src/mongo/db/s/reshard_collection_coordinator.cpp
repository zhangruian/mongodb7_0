/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/s/reshard_collection_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"


namespace mongo {

ReshardCollectionCoordinator::ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                           const BSONObj& initialState)
    : ReshardCollectionCoordinator(service, initialState, true /* persistCoordinatorDocument */) {}

ReshardCollectionCoordinator::ReshardCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                           const BSONObj& initialState,
                                                           bool persistCoordinatorDocument)
    : ShardingDDLCoordinator(service, initialState),
      _initialState(initialState.getOwned()),
      _doc(ReshardCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("ReshardCollectionCoordinatorDocument"), _initialState)),
      _persistCoordinatorDocument(persistCoordinatorDocument) {}

void ReshardCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = ReshardCollectionCoordinatorDocument::parse(
        IDLParserErrorContext("ReshardCollectionCoordinatorDocument"), doc);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another reshard collection with different arguments is already running for the same "
            "namespace",
            SimpleBSONObjComparator::kInstance.evaluate(
                _doc.getReshardCollectionRequest().toBSON() ==
                otherDoc.getReshardCollectionRequest().toBSON()));
}

boost::optional<BSONObj> ReshardCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    cmdBob.appendElements(_doc.getReshardCollectionRequest().toBSON());

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "ReshardCollectionCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("active", true);
    return bob.obj();
}

void ReshardCollectionCoordinator::_enterPhase(Phase newPhase) {
    if (!_persistCoordinatorDocument) {
        return;
    }

    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(6206400,
                2,
                "Reshard collection coordinator phase transition",
                "namespace"_attr = nss(),
                "newPhase"_attr = ReshardCollectionCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = ReshardCollectionCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        _doc = _insertStateDocument(std::move(newDoc));
        return;
    }
    _doc = _updateStateDocument(cc().makeOperationContext().get(), std::move(newDoc));
}

ExecutorFuture<void> ReshardCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kReshard,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                ConfigsvrReshardCollection configsvrReshardCollection(nss(), _doc.getKey());
                configsvrReshardCollection.setDbName(nss().db());
                configsvrReshardCollection.setUnique(_doc.getUnique());
                configsvrReshardCollection.setCollation(_doc.getCollation());
                configsvrReshardCollection.set_presetReshardedChunks(
                    _doc.get_presetReshardedChunks());
                configsvrReshardCollection.setZones(_doc.getZones());
                configsvrReshardCollection.setNumInitialChunks(_doc.getNumInitialChunks());

                const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

                const auto cmdResponse =
                    uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                        NamespaceString::kAdminDb.toString(),
                        CommandHelpers::appendMajorityWriteConcern(
                            configsvrReshardCollection.toBSON({}), opCtx->getWriteConcern()),
                        Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(6206401,
                        "Error running reshard collection",
                        "namespace"_attr = nss(),
                        "error"_attr = redact(status));
            return status;
        });
}

ReshardCollectionCoordinator_NORESILIENT::ReshardCollectionCoordinator_NORESILIENT(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : ReshardCollectionCoordinator(service, initialState, false /* persistCoordinatorDocument */) {}

}  // namespace mongo

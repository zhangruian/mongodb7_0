/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/catalog/coll_mod_index.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/version/releases.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterDatabaseLock);

void assertMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    Lock::DBLock dblock(opCtx, nss.db(), MODE_IS);
    auto dss = DatabaseShardingState::get(opCtx, nss.db().toString());
    if (!dss) {
        return;
    }

    auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
    try {
        const auto collDesc =
            CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx);
        if (!collDesc.isSharded()) {
            auto mpsm = dss->getMovePrimarySourceManager(dssLock);

            if (mpsm) {
                LOGV2(4945200, "assertMovePrimaryInProgress", "namespace"_attr = nss.toString());

                uasserted(ErrorCodes::MovePrimaryInProgress,
                          "movePrimary is in progress for namespace " + nss.toString());
            }
        }
    } catch (const DBException& ex) {
        if (ex.toStatus() != ErrorCodes::MovePrimaryInProgress) {
            LOGV2(4945201, "Error when getting collection description", "what"_attr = ex.what());
            return;
        }
        throw;
    }
}

struct CollModRequest {
    CollModIndexRequest indexRequest;
    BSONElement clusteredIndexExpireAfterSeconds = {};
    BSONElement viewPipeLine = {};
    BSONElement timeseries = {};
    std::string viewOn = {};
    boost::optional<Collection::Validator> collValidator;
    boost::optional<ValidationActionEnum> collValidationAction;
    boost::optional<ValidationLevelEnum> collValidationLevel;
    bool recordPreImages = false;
    boost::optional<ChangeStreamPreAndPostImagesOptions> changeStreamPreAndPostImagesOptions;
};

StatusWith<CollModRequest> parseCollModRequest(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const CollectionPtr& coll,
                                               const BSONObj& cmdObj,
                                               BSONObjBuilder* oplogEntryBuilder) {

    bool isView = !coll;
    bool isTimeseries = coll && coll->getTimeseriesOptions() != boost::none;

    CollModRequest cmr;

    BSONForEach(e, cmdObj) {
        const auto fieldName = e.fieldNameStringData();
        if (isGenericArgument(fieldName)) {
            continue;  // Don't add to oplog builder.
        } else if (fieldName == "collMod") {
            // no-op
        } else if (fieldName == "index" && !isView) {
            BSONObj indexObj = e.Obj();
            StringData indexName;
            BSONObj keyPattern;

            for (auto&& elem : indexObj) {
                const auto field = elem.fieldNameStringData();
                if (field != "name" && field != "keyPattern" && field != "expireAfterSeconds" &&
                    field != "hidden") {
                    return {ErrorCodes::InvalidOptions,
                            str::stream()
                                << "Unrecognized field '" << field << "' in 'index' option"};
                }
            }

            BSONElement nameElem = indexObj["name"];
            BSONElement keyPatternElem = indexObj["keyPattern"];
            if (nameElem && keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Cannot specify both key pattern and name.");
            }

            if (!nameElem && !keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Must specify either index name or key pattern.");
            }

            if (nameElem) {
                if (nameElem.type() != BSONType::String) {
                    return Status(ErrorCodes::InvalidOptions, "Index name must be a string.");
                }
                indexName = nameElem.valueStringData();
            }

            if (keyPatternElem) {
                if (keyPatternElem.type() != BSONType::Object) {
                    return Status(ErrorCodes::InvalidOptions, "Key pattern must be an object.");
                }
                keyPattern = keyPatternElem.embeddedObject();
            }

            auto cmrIndex = &cmr.indexRequest;
            cmrIndex->indexExpireAfterSeconds = indexObj["expireAfterSeconds"];
            cmrIndex->indexHidden = indexObj["hidden"];

            if (cmrIndex->indexExpireAfterSeconds.eoo() && cmrIndex->indexHidden.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds or hidden field");
            }
            if (!cmrIndex->indexExpireAfterSeconds.eoo()) {
                if (isTimeseries) {
                    return Status(ErrorCodes::InvalidOptions,
                                  "TTL indexes are not supported for time-series collections. "
                                  "Please refer to the documentation and use the top-level "
                                  "'expireAfterSeconds' option instead");
                }
                if (auto status = index_key_validate::validateExpireAfterSeconds(
                        cmrIndex->indexExpireAfterSeconds.safeNumberLong());
                    !status.isOK()) {
                    return {ErrorCodes::InvalidOptions, status.reason()};
                }
            }
            if (!cmrIndex->indexHidden.eoo() && !cmrIndex->indexHidden.isBoolean()) {
                return Status(ErrorCodes::InvalidOptions, "hidden field must be a boolean");
            }
            if (!indexName.empty()) {
                cmrIndex->idx = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
                if (!cmrIndex->idx) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << indexName << " for ns " << nss);
                }
            } else {
                std::vector<const IndexDescriptor*> indexes;
                coll->getIndexCatalog()->findIndexesByKeyPattern(
                    opCtx, keyPattern, false, &indexes);

                if (indexes.size() > 1) {
                    return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                                  str::stream() << "index keyPattern " << keyPattern << " matches "
                                                << indexes.size() << " indexes,"
                                                << " must use index name. "
                                                << "Conflicting indexes:" << indexes[0]->infoObj()
                                                << ", " << indexes[1]->infoObj());
                } else if (indexes.empty()) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << keyPattern << " for ns " << nss);
                }

                cmrIndex->idx = indexes[0];
            }

            if (!cmrIndex->indexExpireAfterSeconds.eoo()) {
                BSONElement oldExpireSecs = cmrIndex->idx->infoObj().getField("expireAfterSeconds");
                if (oldExpireSecs.eoo()) {
                    if (cmrIndex->idx->isIdIndex()) {
                        return Status(ErrorCodes::InvalidOptions,
                                      "the _id field does not support TTL indexes");
                    }
                    if (cmrIndex->idx->getNumFields() != 1) {
                        return Status(ErrorCodes::InvalidOptions,
                                      "TTL indexes are single-field indexes, compound indexes do "
                                      "not support TTL");
                    }
                } else if (!oldExpireSecs.isNumber()) {
                    return Status(ErrorCodes::InvalidOptions,
                                  "existing expireAfterSeconds field is not a number");
                }
            }

            if (cmrIndex->indexHidden) {
                // Hiding a hidden index or unhiding a visible index should be treated as a no-op.
                if (cmrIndex->idx->hidden() == cmrIndex->indexHidden.booleanSafe()) {
                    // If the collMod includes "expireAfterSeconds", remove the no-op "hidden"
                    // parameter and write the remaining "index" object to the oplog entry builder.
                    if (!cmrIndex->indexExpireAfterSeconds.eoo()) {
                        oplogEntryBuilder->append(fieldName, indexObj.removeField("hidden"));
                    }
                    // Un-set "indexHidden" in CollModRequest, and skip the automatic write to the
                    // oplogEntryBuilder that occurs at the end of the parsing loop.
                    cmrIndex->indexHidden = {};
                    continue;
                }

                // Disallow index hiding/unhiding on system collections.
                // Bucket collections, which hold data for user-created time-series collections, do
                // not have this restriction.
                if (nss.isSystem() && !nss.isTimeseriesBucketsCollection()) {
                    return Status(ErrorCodes::BadValue, "Can't hide index on system collection");
                }

                // Disallow index hiding/unhiding on _id indexes - these are created by default and
                // are critical to most collection operations.
                if (cmrIndex->idx->isIdIndex()) {
                    return Status(ErrorCodes::BadValue, "can't hide _id index");
                }
            }
        } else if (fieldName == "validator" && !isView && !isTimeseries) {
            // If the feature compatibility version is not kLatest, and we are validating features
            // as primary, ban the use of new agg features introduced in kLatest to prevent them
            // from being persisted in the catalog.
            boost::optional<multiversion::FeatureCompatibilityVersion>
                maxFeatureCompatibilityVersion;
            // (Generic FCV reference): This FCV check should exist across LTS binary versions.
            multiversion::FeatureCompatibilityVersion fcv;
            if (serverGlobalParams.validateFeaturesAsPrimary.load() &&
                serverGlobalParams.featureCompatibility.isLessThan(
                    multiversion::GenericFCV::kLatest, &fcv)) {
                maxFeatureCompatibilityVersion = fcv;
            }
            cmr.collValidator = coll->parseValidator(opCtx,
                                                     e.Obj().getOwned(),
                                                     MatchExpressionParser::kDefaultSpecialFeatures,
                                                     maxFeatureCompatibilityVersion);
            if (!cmr.collValidator->isOK()) {
                return cmr.collValidator->getStatus();
            }
        } else if (fieldName == "validationLevel" && !isView && !isTimeseries) {
            try {
                cmr.collValidationLevel = ValidationLevel_parse({"validationLevel"}, e.String());
            } catch (const DBException& exc) {
                return exc.toStatus();
            }
        } else if (fieldName == "validationAction" && !isView && !isTimeseries) {
            try {
                cmr.collValidationAction = ValidationAction_parse({"validationAction"}, e.String());
            } catch (const DBException& exc) {
                return exc.toStatus();
            }
        } else if (fieldName == "pipeline") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'pipeline' option only supported on a view");
            }
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::InvalidOptions, "not a valid aggregation pipeline");
            }
            cmr.viewPipeLine = e;
        } else if (fieldName == "viewOn") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'viewOn' option only supported on a view");
            }
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::InvalidOptions, "'viewOn' option must be a string");
            }
            cmr.viewOn = e.str();
        } else if (fieldName == "recordPreImages" && !isView && !isTimeseries) {
            cmr.recordPreImages = e.trueValue();
        } else if (fieldName == CollMod::kChangeStreamPreAndPostImagesFieldName && !isView &&
                   !isTimeseries) {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "'" << CollMod::kChangeStreamPreAndPostImagesFieldName
                                      << "' option must be a document"};
            }

            try {
                cmr.changeStreamPreAndPostImagesOptions =
                    ChangeStreamPreAndPostImagesOptions::parse(
                        {"changeStreamPreAndPostImagesOptions"}, e.Obj());
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (fieldName == "expireAfterSeconds") {
            if (coll->getRecordStore()->keyFormat() != KeyFormat::String) {
                return Status(ErrorCodes::InvalidOptions,
                              "'expireAfterSeconds' option is only supported on collections "
                              "clustered by _id");
            }

            if (e.type() == mongo::String) {
                const std::string elemStr = e.String();
                if (elemStr != "off") {
                    return Status(
                        ErrorCodes::InvalidOptions,
                        str::stream()
                            << "Invalid string value for the 'clusteredIndex::expireAfterSeconds' "
                            << "option. Got: '" << elemStr << "'. Accepted value is 'off'");
                }
            } else {
                invariant(e.type() == mongo::NumberLong);
                const int64_t elemNum = e.safeNumberLong();
                uassertStatusOK(index_key_validate::validateExpireAfterSeconds(elemNum));
            }

            cmr.clusteredIndexExpireAfterSeconds = e;
        } else if (fieldName == "timeseries") {
            if (!isTimeseries) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option only supported on a time-series collection: "
                                            << fieldName);
            }

            cmr.timeseries = e;
        } else {
            if (isTimeseries) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option not supported on a time-series collection: "
                                            << fieldName);
            }

            if (isView) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option not supported on a view: " << fieldName);
            }

            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "unknown option to collMod: " << fieldName);
        }

        oplogEntryBuilder->append(e);
    }

    return {std::move(cmr)};
}

void _setClusteredExpireAfterSeconds(OperationContext* opCtx,
                                     const CollectionOptions& oldCollOptions,
                                     Collection* coll,
                                     const BSONElement& clusteredIndexExpireAfterSeconds) {
    invariant(oldCollOptions.clusteredIndex);

    boost::optional<int64_t> oldExpireAfterSeconds = oldCollOptions.expireAfterSeconds;

    if (clusteredIndexExpireAfterSeconds.type() == mongo::String) {
        const std::string newExpireAfterSeconds = clusteredIndexExpireAfterSeconds.String();
        invariant(newExpireAfterSeconds == "off");
        if (!oldExpireAfterSeconds) {
            // expireAfterSeconds is already disabled on the clustered index.
            return;
        }

        coll->updateClusteredIndexTTLSetting(opCtx, boost::none);
        return;
    }

    invariant(clusteredIndexExpireAfterSeconds.type() == mongo::NumberLong);
    int64_t newExpireAfterSeconds = clusteredIndexExpireAfterSeconds.safeNumberLong();
    if (oldExpireAfterSeconds && *oldExpireAfterSeconds == newExpireAfterSeconds) {
        // expireAfterSeconds is already the requested value on the clustered index.
        return;
    }

    // If this collection was not previously TTL, inform the TTL monitor when we commit.
    if (!oldExpireAfterSeconds) {
        auto ttlCache = &TTLCollectionCache::get(opCtx->getServiceContext());
        opCtx->recoveryUnit()->onCommit([ttlCache, uuid = coll->uuid()](auto _) {
            ttlCache->registerTTLInfo(uuid, TTLCollectionCache::ClusteredId());
        });
    }

    invariant(newExpireAfterSeconds >= 0);
    coll->updateClusteredIndexTTLSetting(opCtx, newExpireAfterSeconds);
}

Status _collModInternal(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result) {
    StringData dbName = nss.db();
    AutoGetCollection coll(opCtx, nss, MODE_X, AutoGetCollectionViewMode::kViewsPermitted);
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_X);

    Database* const db = coll.getDb();

    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangAfterDatabaseLock, opCtx, "hangAfterDatabaseLock", []() {}, nss);

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (db && !coll) {
        const auto sharedView = ViewCatalog::get(db)->lookup(opCtx, nss);
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    if (coll) {
        assertMovePrimaryInProgress(opCtx, nss);
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
        CollectionShardingState::get(opCtx, nss)
            ->getCollectionDescription(opCtx)
            .throwIfReshardingInProgress(nss);
    }


    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
        if (nss.isTimeseriesBucketsCollection()) {
            // If a sharded time-series collection is dropped, it's possible that a stale mongos
            // sends the request on the buckets namespace instead of the view namespace. Ensure that
            // the shardVersion is upto date before throwing an error.
            CollectionShardingState::get(opCtx, nss)->checkShardVersionOrThrow(opCtx);
        }
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp, update the Top stats, and check shard version if the
    // operation is not on a view.
    OldClientContext ctx(opCtx, nss.ns(), !view);

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while setting collection options on " << nss);
    }

    BSONObjBuilder oplogEntryBuilder;
    auto statusW =
        parseCollModRequest(opCtx, nss, coll.getCollection(), cmdObj, &oplogEntryBuilder);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }
    auto oplogEntryObj = oplogEntryBuilder.obj();

    // Save both states of the CollModRequest to allow writeConflictRetries.
    CollModRequest cmrNew = std::move(statusW.getValue());
    auto viewPipeline = cmrNew.viewPipeLine;
    auto viewOn = cmrNew.viewOn;
    auto clusteredIndexExpireAfterSeconds = cmrNew.clusteredIndexExpireAfterSeconds;
    // WriteConflictExceptions thrown in the writeConflictRetry loop enclosing this function can
    // cause collModIndexRequest->idx to become invalid, so save a copy to use in the loop until we
    // can refresh it.
    auto idx = cmrNew.indexRequest.idx;
    auto ts = cmrNew.timeseries;

    if (!serverGlobalParams.quiet.load()) {
        LOGV2(5324200, "CMD: collMod", "cmdObj"_attr = cmdObj);
    }

    return writeConflictRetry(opCtx, "collMod", nss.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        // Handle collMod on a view and return early. The View Catalog handles the creation of oplog
        // entries for modifications on a view.
        if (view) {
            if (viewPipeline)
                view->setPipeline(viewPipeline);

            if (!viewOn.empty())
                view->setViewOn(NamespaceString(dbName, viewOn));

            BSONArrayBuilder pipeline;
            for (auto& item : view->pipeline()) {
                pipeline.append(item);
            }
            auto errorStatus =
                ViewCatalog::modifyView(opCtx, db, nss, view->viewOn(), BSONArray(pipeline.obj()));
            if (!errorStatus.isOK()) {
                return errorStatus;
            }

            wunit.commit();
            return Status::OK();
        }

        // In order to facilitate the replication rollback process, which makes a best effort
        // attempt to "undo" a set of oplog operations, we store a snapshot of the old collection
        // options to provide to the OpObserver. TTL index updates aren't a part of collection
        // options so we save the relevant TTL index data in a separate object.

        const CollectionOptions& oldCollOptions = coll->getCollectionOptions();

        // TODO SERVER-58584: remove the feature flag.
        if (feature_flags::gFeatureFlagChangeStreamPreAndPostImages.isEnabledAndIgnoreFCV()) {
            // If 'changeStreamPreAndPostImagesOptions' are enabled, 'recordPreImages' must be set
            // to false. If 'recordPreImages' is set to true, 'changeStreamPreAndPostImagesOptions'
            // must be disabled.
            if (cmrNew.changeStreamPreAndPostImagesOptions &&
                cmrNew.changeStreamPreAndPostImagesOptions->getEnabled()) {
                cmrNew.recordPreImages = false;
            }

            if (cmrNew.recordPreImages) {
                cmrNew.changeStreamPreAndPostImagesOptions =
                    ChangeStreamPreAndPostImagesOptions(false);
            }
        }

        boost::optional<IndexCollModInfo> indexCollModInfo;

        // Handle collMod operation type appropriately.
        if (clusteredIndexExpireAfterSeconds) {
            _setClusteredExpireAfterSeconds(opCtx,
                                            oldCollOptions,
                                            coll.getWritableCollection(),
                                            clusteredIndexExpireAfterSeconds);
        }

        // Handle index modifications.
        processCollModIndexRequest(
            opCtx, &coll, idx, &cmrNew.indexRequest, &indexCollModInfo, result);

        if (cmrNew.collValidator) {
            coll.getWritableCollection()->setValidator(opCtx, *cmrNew.collValidator);
        }
        if (cmrNew.collValidationAction)
            uassertStatusOKWithContext(coll.getWritableCollection()->setValidationAction(
                                           opCtx, *cmrNew.collValidationAction),
                                       "Failed to set validationAction");
        if (cmrNew.collValidationLevel) {
            uassertStatusOKWithContext(coll.getWritableCollection()->setValidationLevel(
                                           opCtx, *cmrNew.collValidationLevel),
                                       "Failed to set validationLevel");
        }

        if (cmrNew.recordPreImages != oldCollOptions.recordPreImages) {
            coll.getWritableCollection()->setRecordPreImages(opCtx, cmrNew.recordPreImages);
        }

        // TODO SERVER-58584: remove the feature flag.
        if (feature_flags::gFeatureFlagChangeStreamPreAndPostImages.isEnabledAndIgnoreFCV() &&
            cmrNew.changeStreamPreAndPostImagesOptions.has_value() &&
            *cmrNew.changeStreamPreAndPostImagesOptions !=
                oldCollOptions.changeStreamPreAndPostImagesOptions) {
            coll.getWritableCollection()->setChangeStreamPreAndPostImages(
                opCtx, *cmrNew.changeStreamPreAndPostImagesOptions);
        }

        if (ts.isABSONObj()) {
            auto res = timeseries::applyTimeseriesOptionsModifications(*oldCollOptions.timeseries,
                                                                       ts.Obj());
            uassertStatusOK(res);
            auto [newOptions, changed] = res.getValue();
            if (changed) {
                coll.getWritableCollection()->setTimeseriesOptions(opCtx, newOptions);
            }
        }

        // Remove any invalid index options for indexes belonging to this collection.
        std::vector<std::string> indexesWithInvalidOptions =
            coll.getWritableCollection()->removeInvalidIndexOptions(opCtx);
        for (const auto& indexWithInvalidOptions : indexesWithInvalidOptions) {
            const IndexDescriptor* desc =
                coll->getIndexCatalog()->findIndexByName(opCtx, indexWithInvalidOptions);
            invariant(desc);

            // Notify the index catalog that the definition of this index changed.
            coll.getWritableCollection()->getIndexCatalog()->refreshEntry(
                opCtx, coll.getWritableCollection(), desc);
        }

        // Only observe non-view collMods, as view operations are observed as operations on the
        // system.views collection.
        auto* const opObserver = opCtx->getServiceContext()->getOpObserver();
        opObserver->onCollMod(
            opCtx, nss, coll->uuid(), oplogEntryObj, oldCollOptions, indexCollModInfo);

        wunit.commit();
        return Status::OK();
    });
}

}  // namespace

Status collMod(OperationContext* opCtx,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    return _collModInternal(opCtx, nss, cmdObj, result);
}

}  // namespace mongo

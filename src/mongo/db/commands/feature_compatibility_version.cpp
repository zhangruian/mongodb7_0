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

#include "mongo/db/commands/feature_compatibility_version.h"

#include "mongo/base/status.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version_document_gen.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;

Lock::ResourceMutex FeatureCompatibilityVersion::fcvLock("featureCompatibilityVersionLock");

MONGO_FAIL_POINT_DEFINE(hangBeforeAbortingRunningTransactionsOnFCVDowngrade);

namespace {
bool isWriteableStorageEngine() {
    return !storageGlobalParams.readOnly && (storageGlobalParams.engine != "devnull");
}

// Returns the featureCompatibilityVersion document if it exists.
boost::optional<BSONObj> findFcvDocument(OperationContext* opCtx) {
    // Ensure database is opened and exists.
    AutoGetOrCreateDb autoDb(opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_IX);

    const auto query = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (!swFcv.isOK()) {
        return boost::none;
    }
    return swFcv.getValue();
}

/**
 * Build update command for featureCompatibilityVersion document updates.
 */
void runUpdateCommand(OperationContext* opCtx, const FeatureCompatibilityVersionDocument& fcvDoc) {
    DBDirectClient client(opCtx);
    NamespaceString nss(NamespaceString::kServerConfigurationNamespace);

    BSONObjBuilder updateCmd;
    updateCmd.append("update", nss.coll());
    {
        BSONArrayBuilder updates(updateCmd.subarrayStart("updates"));
        {
            BSONObjBuilder updateSpec(updates.subobjStart());
            {
                BSONObjBuilder queryFilter(updateSpec.subobjStart("q"));
                queryFilter.append("_id", FeatureCompatibilityVersionParser::kParameterName);
            }
            {
                BSONObjBuilder updateMods(updateSpec.subobjStart("u"));
                fcvDoc.serialize(&updateMods);
            }
            updateSpec.appendBool("upsert", true);
        }
    }
    auto timeout = opCtx->getWriteConcern().usedDefault ? WriteConcernOptions::kNoTimeout
                                                        : opCtx->getWriteConcern().wTimeout;
    auto newWC = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout);
    updateCmd.append(WriteConcernOptions::kWriteConcernField, newWC.toBSON());

    // Update the featureCompatibilityVersion document stored in the server configuration
    // collection.
    BSONObj updateResult;
    client.runCommand(nss.db().toString(), updateCmd.obj(), updateResult);
    uassertStatusOK(getStatusFromWriteCommandReply(updateResult));
}
}  // namespace

void FeatureCompatibilityVersion::setTargetUpgradeFrom(
    OperationContext* opCtx, ServerGlobalParams::FeatureCompatibility::Version fromVersion) {
    // Sets both 'version' and 'targetVersion' fields.
    FeatureCompatibilityVersionDocument fcvDoc;
    fcvDoc.setVersion(fromVersion);
    fcvDoc.setTargetVersion(ServerGlobalParams::FeatureCompatibility::kLatest);
    runUpdateCommand(opCtx, fcvDoc);
}

void FeatureCompatibilityVersion::setTargetDowngrade(
    OperationContext* opCtx, ServerGlobalParams::FeatureCompatibility::Version version) {
    // Sets 'version', 'targetVersion' and 'previousVersion' fields.
    FeatureCompatibilityVersionDocument fcvDoc;
    fcvDoc.setVersion(version);
    fcvDoc.setTargetVersion(version);
    fcvDoc.setPreviousVersion(ServerGlobalParams::FeatureCompatibility::kLatest);
    runUpdateCommand(opCtx, fcvDoc);
}

void FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(
    OperationContext* opCtx, ServerGlobalParams::FeatureCompatibility::Version version) {
    // Updates 'version' field, while also unsetting the 'targetVersion' field and the
    // 'previousVersion' field.
    FeatureCompatibilityVersionDocument fcvDoc;
    fcvDoc.setVersion(version);
    runUpdateCommand(opCtx, fcvDoc);
}

void FeatureCompatibilityVersion::setIfCleanStartup(OperationContext* opCtx,
                                                    repl::StorageInterface* storageInterface) {
    if (!isCleanStartUp())
        return;

    // If the server was not started with --shardsvr, the default featureCompatibilityVersion on
    // clean startup is the upgrade version. If it was started with --shardsvr, the default
    // featureCompatibilityVersion is the downgrade version, so that it can be safely added to a
    // downgrade version cluster. The config server will run setFeatureCompatibilityVersion as part
    // of addShard.
    const bool storeUpgradeVersion = serverGlobalParams.clusterRole != ClusterRole::ShardServer;

    UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
    NamespaceString nss(NamespaceString::kServerConfigurationNamespace);

    {
        CollectionOptions options;
        options.uuid = CollectionUUID::gen();
        uassertStatusOK(storageInterface->createCollection(opCtx, nss, options));
    }

    FeatureCompatibilityVersionDocument fcvDoc;
    if (storeUpgradeVersion) {
        fcvDoc.setVersion(ServerGlobalParams::FeatureCompatibility::kLatest);
    } else {
        fcvDoc.setVersion(ServerGlobalParams::FeatureCompatibility::kLastLTS);
    }

    // We then insert the featureCompatibilityVersion document into the server configuration
    // collection. The server parameter will be updated on commit by the op observer.
    uassertStatusOK(storageInterface->insertDocument(
        opCtx,
        nss,
        repl::TimestampedBSONObj{fcvDoc.toBSON(), Timestamp()},
        repl::OpTime::kUninitializedTerm));  // No timestamp or term because this write is not
                                             // replicated.
}

bool FeatureCompatibilityVersion::isCleanStartUp() {
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    std::vector<std::string> dbNames = storageEngine->listDatabases();

    for (auto&& dbName : dbNames) {
        if (dbName != "local") {
            return false;
        }
    }
    return true;
}

void FeatureCompatibilityVersion::onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc) {
    auto idElement = doc["_id"];
    if (idElement.type() != BSONType::String ||
        idElement.String() != FeatureCompatibilityVersionParser::kParameterName) {
        return;
    }
    auto newVersion = uassertStatusOK(FeatureCompatibilityVersionParser::parse(doc));

    // To avoid extra log messages when the targetVersion is set/unset, only log when the version
    // changes.
    logv2::DynamicAttributes attrs;
    bool isDifferent = true;
    if (serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        const auto currentVersion = serverGlobalParams.featureCompatibility.getVersion();
        attrs.add("currentVersion", FeatureCompatibilityVersionParser::toString(currentVersion));
        isDifferent = currentVersion != newVersion;
    }

    if (isDifferent) {
        attrs.add("newVersion", FeatureCompatibilityVersionParser::toString(newVersion));
        LOGV2(20459, "Setting featureCompatibilityVersion", attrs);
    }

    opCtx->recoveryUnit()->onCommit(
        [opCtx, newVersion](boost::optional<Timestamp>) { _setVersion(opCtx, newVersion); });
}

void FeatureCompatibilityVersion::updateMinWireVersion() {
    WireSpec& spec = WireSpec::instance();

    switch (serverGlobalParams.featureCompatibility.getVersion()) {
        case ServerGlobalParams::FeatureCompatibility::kLatest:
        case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingFrom44To451:
        case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingFrom451To44:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
            return;
        case ServerGlobalParams::FeatureCompatibility::kLastLTS:
            spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION - 1;
            spec.outgoing.minWireVersion = LATEST_WIRE_VERSION - 1;
            return;
        case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault44Behavior:
            // getVersion() does not return this value.
            MONGO_UNREACHABLE;
    }
}

void FeatureCompatibilityVersion::initializeForStartup(OperationContext* opCtx) {
    // Global write lock must be held.
    invariant(opCtx->lockState()->isW());
    auto featureCompatibilityVersion = findFcvDocument(opCtx);
    if (!featureCompatibilityVersion) {
        return;
    }

    // If the server configuration collection already contains a valid featureCompatibilityVersion
    // document, cache it in-memory as a server parameter.
    auto swVersion = FeatureCompatibilityVersionParser::parse(*featureCompatibilityVersion);

    // Note this error path captures all cases of an FCV document existing, but with any
    // unacceptable value. This includes unexpected cases with no path forward such as the FCV value
    // not being a string.
    if (!swVersion.isOK()) {
        uassertStatusOK({ErrorCodes::MustDowngrade,
                         str::stream()
                             << "UPGRADE PROBLEM: Found an invalid featureCompatibilityVersion "
                                "document (ERROR: "
                             << swVersion.getStatus()
                             << "). If the current featureCompatibilityVersion is below 4.4, see "
                                "the documentation on upgrading at "
                             << feature_compatibility_version_documentation::kUpgradeLink << "."});
    }

    auto version = swVersion.getValue();
    serverGlobalParams.featureCompatibility.setVersion(version);
    FeatureCompatibilityVersion::updateMinWireVersion();

    // On startup, if the version is in an upgrading or downgrading state, print a warning.
    if (version == ServerGlobalParams::FeatureCompatibility::Version::kUpgradingFrom44To451) {
        LOGV2_WARNING_OPTIONS(
            21011,
            {logv2::LogTag::kStartupWarnings},
            "A featureCompatibilityVersion upgrade did not complete. The current "
            "featureCompatibilityVersion is {currentfeatureCompatibilityVersion}. To fix this, "
            "use the setFeatureCompatibilityVersion command to resume upgrade to 4.5.1",
            "A featureCompatibilityVersion upgrade did not complete. To fix this, use the "
            "setFeatureCompatibilityVersion command to resume upgrade to 4.5.1",
            "currentfeatureCompatibilityVersion"_attr =
                FeatureCompatibilityVersionParser::toString(version));
    } else if (version ==
               ServerGlobalParams::FeatureCompatibility::Version::kDowngradingFrom451To44) {
        LOGV2_WARNING_OPTIONS(
            21014,
            {logv2::LogTag::kStartupWarnings},
            "A featureCompatibilityVersion downgrade did not complete. The current "
            "featureCompatibilityVersion is {currentfeatureCompatibilityVersion}. To fix this, "
            "use the setFeatureCompatibilityVersion command to resume downgrade to 4.4.",
            "A featureCompatibilityVersion downgrade did not complete. To fix this, use the "
            "setFeatureCompatibilityVersion command to resume downgrade to 4.5.1",
            "currentfeatureCompatibilityVersion"_attr =
                FeatureCompatibilityVersionParser::toString(version));
    }
}

// Fatally asserts if the featureCompatibilityVersion document is not initialized, when required.
void FeatureCompatibilityVersion::fassertInitializedAfterStartup(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);
    const auto replProcess = repl::ReplicationProcess::get(opCtx);
    const auto& replSettings = repl::ReplicationCoordinator::get(opCtx)->getSettings();

    // The node did not complete the last initial sync. If the initial sync flag is set and we are
    // part of a replica set, we expect the version to be initialized as part of initial sync after
    // startup.
    bool needInitialSync = replSettings.usingReplSets() && replProcess &&
        replProcess->getConsistencyMarkers()->getInitialSyncFlag(opCtx);
    if (needInitialSync) {
        return;
    }

    auto fcvDocument = findFcvDocument(opCtx);

    auto const storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto dbNames = storageEngine->listDatabases();
    bool nonLocalDatabases = std::any_of(dbNames.begin(), dbNames.end(), [](auto name) {
        return name != NamespaceString::kLocalDb;
    });

    // Fail to start up if there is no featureCompatibilityVersion document and there are non-local
    // databases present.
    if (!fcvDocument && nonLocalDatabases) {
        LOGV2_FATAL_NOTRACE(40652,
                            "Unable to start up mongod due to missing featureCompatibilityVersion "
                            "document. Please run with --repair to restore the document.");
    }

    // If we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the in-memory
    // featureCompatibilityVersion parameter to still be uninitialized until after startup.
    if (isWriteableStorageEngine() && (!replSettings.usingReplSets() || nonLocalDatabases)) {
        invariant(serverGlobalParams.featureCompatibility.isVersionInitialized());
    }
}

void FeatureCompatibilityVersion::_setVersion(
    OperationContext* opCtx, ServerGlobalParams::FeatureCompatibility::Version newVersion) {
    serverGlobalParams.featureCompatibility.setVersion(newVersion);
    updateMinWireVersion();

    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (newVersion != ServerGlobalParams::FeatureCompatibility::kLastLTS) {
        // Close all incoming connections from internal clients with binary versions lower than
        // ours.
        opCtx->getServiceContext()->getServiceEntryPoint()->endAllSessions(
            transport::Session::kLatestVersionInternalClientKeepOpen |
            transport::Session::kExternalClientKeepOpen);
        // Close all outgoing connections to servers with binary versions lower than ours.
        executor::EgressTagCloserManager::get(opCtx->getServiceContext())
            .dropConnections(transport::Session::kKeepOpen);
    }

    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (newVersion != ServerGlobalParams::FeatureCompatibility::kLatest) {
        if (MONGO_unlikely(hangBeforeAbortingRunningTransactionsOnFCVDowngrade.shouldFail())) {
            LOGV2(20460,
                  "FeatureCompatibilityVersion - "
                  "hangBeforeAbortingRunningTransactionsOnFCVDowngrade fail point enabled, "
                  "blocking until fail point is disabled");
            hangBeforeAbortingRunningTransactionsOnFCVDowngrade.pauseWhileSet();
        }
        // Abort all open transactions when downgrading the featureCompatibilityVersion.
        SessionKiller::Matcher matcherAllSessions(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        killSessionsAbortUnpreparedTransactions(opCtx, matcherAllSessions);
    }
    const auto replCoordinator = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoordinator->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    // We only want to increment the server TopologyVersion when the minWireVersion has changed.
    // This can only happen in two scenarios:
    // 1. Setting featureCompatibilityVersion from downgrading to fullyDowngraded.
    // 2. Setting featureCompatibilityVersion from fullyDowngraded to upgrading.
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    const auto shouldIncrementTopologyVersion =
        newVersion == ServerGlobalParams::FeatureCompatibility::kLastLTS ||
        newVersion == ServerGlobalParams::FeatureCompatibility::Version::kUpgradingFrom44To451;
    if (isReplSet && shouldIncrementTopologyVersion) {
        replCoordinator->incrementTopologyVersion();
    }
}

void FeatureCompatibilityVersion::onReplicationRollback(OperationContext* opCtx) {
    const auto query = BSON("_id" << FeatureCompatibilityVersionParser::kParameterName);
    const auto swFcv = repl::StorageInterface::get(opCtx)->findById(
        opCtx, NamespaceString::kServerConfigurationNamespace, query["_id"]);
    if (swFcv.isOK()) {
        const auto featureCompatibilityVersion = swFcv.getValue();
        auto swVersion = FeatureCompatibilityVersionParser::parse(featureCompatibilityVersion);
        const auto memoryFcv = serverGlobalParams.featureCompatibility.getVersion();
        if (swVersion.isOK() && (swVersion.getValue() != memoryFcv)) {
            auto diskFcv = swVersion.getValue();
            LOGV2(4675801,
                  "Setting featureCompatibilityVersion as part of rollback",
                  "newVersion"_attr = FeatureCompatibilityVersionParser::toString(diskFcv),
                  "oldVersion"_attr = FeatureCompatibilityVersionParser::toString(memoryFcv));
            _setVersion(opCtx, diskFcv);
        }
    }
}

/**
 * Read-only server parameter for featureCompatibilityVersion.
 */
// No ability to specify 'none' as set_at type,
// so use 'startup' in the IDL file, then override to none here.
FeatureCompatibilityVersionParameter::FeatureCompatibilityVersionParameter(StringData name,
                                                                           ServerParameterType)
    : ServerParameter(ServerParameterSet::getGlobal(), name, false, false) {}

void FeatureCompatibilityVersionParameter::append(OperationContext* opCtx,
                                                  BSONObjBuilder& b,
                                                  const std::string& name) {
    uassert(ErrorCodes::UnknownFeatureCompatibilityVersion,
            str::stream() << name << " is not yet known.",
            serverGlobalParams.featureCompatibility.isVersionInitialized());

    FeatureCompatibilityVersionDocument fcvDoc;
    BSONObjBuilder featureCompatibilityVersionBuilder(b.subobjStart(name));
    auto version = serverGlobalParams.featureCompatibility.getVersion();
    switch (version) {
        case ServerGlobalParams::FeatureCompatibility::kLatest:
        case ServerGlobalParams::FeatureCompatibility::kLastLTS:
            fcvDoc.setVersion(version);
            break;
        case ServerGlobalParams::FeatureCompatibility::kUpgradingFromLastLTSToLatest:
            fcvDoc.setVersion(ServerGlobalParams::FeatureCompatibility::kLastLTS);
            fcvDoc.setTargetVersion(ServerGlobalParams::FeatureCompatibility::kLatest);
            break;
        case ServerGlobalParams::FeatureCompatibility::kDowngradingFromLatestToLastLTS:
            fcvDoc.setVersion(ServerGlobalParams::FeatureCompatibility::kLastLTS);
            fcvDoc.setTargetVersion(ServerGlobalParams::FeatureCompatibility::kLastLTS);
            fcvDoc.setPreviousVersion(ServerGlobalParams::FeatureCompatibility::kLatest);
            break;
        case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault44Behavior:
            // getVersion() does not return this value.
            MONGO_UNREACHABLE;
    }
    featureCompatibilityVersionBuilder.appendElements(fcvDoc.toBSON().removeField("_id"));
}

Status FeatureCompatibilityVersionParameter::setFromString(const std::string&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter. See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << "."};
}

}  // namespace mongo

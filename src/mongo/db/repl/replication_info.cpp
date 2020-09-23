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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include <list>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/speculative_auth.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/fail_point.h"

namespace mongo {

// Hangs in the beginning of each isMaster command when set.
MONGO_FAIL_POINT_DEFINE(waitInIsMaster);
// Awaitable isMaster requests with the proper topologyVersions will sleep for maxAwaitTimeMS on
// standalones. This failpoint will hang right before doing this sleep when set.
MONGO_FAIL_POINT_DEFINE(hangWaitingForIsMasterResponseOnStandalone);

using std::list;
using std::string;
using std::stringstream;
using std::unique_ptr;

namespace repl {
namespace {

constexpr auto kHelloString = "hello"_sd;
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;

void appendPrimaryOnlyServiceInfo(ServiceContext* serviceContext, BSONObjBuilder* result) {
    auto registry = PrimaryOnlyServiceRegistry::get(serviceContext);
    registry->reportServiceInfoForServerStatus(result);
}

/**
 * Appends replication-related fields to the isMaster response. Returns the topology version that
 * was included in the response.
 */
TopologyVersion appendReplicationInfo(OperationContext* opCtx,
                                      BSONObjBuilder* result,
                                      bool appendReplicationProcess,
                                      bool useLegacyResponseFields,
                                      boost::optional<TopologyVersion> clientTopologyVersion,
                                      boost::optional<long long> maxAwaitTimeMS) {
    TopologyVersion topologyVersion;
    ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets()) {
        const auto& horizonParams = SplitHorizon::getParameters(opCtx->getClient());

        boost::optional<Date_t> deadline;
        if (maxAwaitTimeMS) {
            deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(*maxAwaitTimeMS);
        }
        auto isMasterResponse =
            replCoord->awaitIsMasterResponse(opCtx, horizonParams, clientTopologyVersion, deadline);
        result->appendElements(isMasterResponse->toBSON(useLegacyResponseFields));
        if (appendReplicationProcess) {
            replCoord->appendSlaveInfoData(result);
        }
        invariant(isMasterResponse->getTopologyVersion());
        return isMasterResponse->getTopologyVersion().get();
    }

    auto currentTopologyVersion = replCoord->getTopologyVersion();

    if (clientTopologyVersion &&
        clientTopologyVersion->getProcessId() == currentTopologyVersion.getProcessId()) {
        uassert(51764,
                str::stream() << "Received a topology version with counter: "
                              << clientTopologyVersion->getCounter()
                              << " which is greater than the server topology version counter: "
                              << currentTopologyVersion.getCounter(),
                clientTopologyVersion->getCounter() == currentTopologyVersion.getCounter());

        // The topologyVersion never changes on a running standalone process, so just sleep for
        // maxAwaitTimeMS.
        invariant(maxAwaitTimeMS);

        IsMasterMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();
        ON_BLOCK_EXIT([&] { IsMasterMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges(); });
        if (MONGO_unlikely(hangWaitingForIsMasterResponseOnStandalone.shouldFail())) {
            // Used in tests that wait for this failpoint to be entered to guarantee that the
            // request is waiting and metrics have been updated.
            LOGV2(31462, "Hanging due to hangWaitingForIsMasterResponseOnStandalone failpoint.");
            hangWaitingForIsMasterResponseOnStandalone.pauseWhileSet(opCtx);
        }
        opCtx->sleepFor(Milliseconds(*maxAwaitTimeMS));
    }

    result->appendBool((useLegacyResponseFields ? "ismaster" : "isWritablePrimary"),
                       ReplicationCoordinator::get(opCtx)->isMasterForReportingPurposes());

    BSONObjBuilder topologyVersionBuilder(result->subobjStart("topologyVersion"));
    currentTopologyVersion.serialize(&topologyVersionBuilder);

    return currentTopologyVersion;
}

class ReplicationInfoServerStatus : public ServerStatusSection {
public:
    ReplicationInfoServerStatus() : ServerStatusSection("repl") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!ReplicationCoordinator::get(opCtx)->isReplEnabled()) {
            return BSONObj();
        }

        bool appendReplicationProcess = configElement.numberInt() > 0;

        BSONObjBuilder result;
        // TODO SERVER-50219: Change useLegacyResponseFields to false once the serverStatus changes
        // to remove master-slave terminology are merged.
        appendReplicationInfo(opCtx,
                              &result,
                              appendReplicationProcess,
                              true /* useLegacyResponseFields */,
                              boost::none /* clientTopologyVersion */,
                              boost::none /* maxAwaitTimeMS */);

        appendPrimaryOnlyServiceInfo(opCtx->getServiceContext(), &result);

        auto rbid = ReplicationProcess::get(opCtx)->getRollbackID();
        if (ReplicationProcess::kUninitializedRollbackId != rbid) {
            result.append("rbid", rbid);
        }

        return result.obj();
    }

} replicationInfoServerStatus;

class OplogInfoServerStatus : public ServerStatusSection {
public:
    OplogInfoServerStatus() : ServerStatusSection("oplog") {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled()) {
            return BSONObj();
        }

        BSONObjBuilder result;
        // TODO(siyuan) Output term of OpTime
        result.append("latestOptime", replCoord->getMyLastAppliedOpTime().getTimestamp());

        auto earliestOplogTimestampFetch = [&] {
            AutoGetOplog oplogRead(opCtx, OplogAccessMode::kRead);
            if (!oplogRead.getCollection()) {
                return StatusWith<Timestamp>(ErrorCodes::NamespaceNotFound, "oplog doesn't exist");
            }
            return oplogRead.getCollection()->getRecordStore()->getEarliestOplogTimestamp(opCtx);
        }();

        if (earliestOplogTimestampFetch.getStatus() == ErrorCodes::OplogOperationUnsupported) {
            // Falling back to use getSingleton if the storage engine does not support
            // getEarliestOplogTimestamp.
            BSONObj o;
            if (Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), o)) {
                earliestOplogTimestampFetch = o["ts"].timestamp();
            }
        }

        uassert(
            17347, "Problem reading earliest entry from oplog", earliestOplogTimestampFetch.isOK());
        result.append("earliestOptime", earliestOplogTimestampFetch.getValue());

        return result.obj();
    }
} oplogInfoServerStatus;

class CmdHello : public BasicCommandWithReplyBuilderInterface {
public:
    CmdHello() : CmdHello(kHelloString, {}) {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    bool requiresAuth() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ hello : 1 }";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {}  // No auth required

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const string&,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        waitInIsMaster.pauseWhileSet(opCtx);

        /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
           authenticated.
        */
        if (cmdObj["forShell"].trueValue()) {
            LastError::get(opCtx->getClient()).disable();
        }

        transport::Session::TagMask sessionTagsToSet = 0;
        transport::Session::TagMask sessionTagsToUnset = 0;

        // Tag connections to avoid closing them on stepdown.
        auto hangUpElement = cmdObj["hangUpOnStepDown"];
        if (!hangUpElement.eoo() && !hangUpElement.trueValue()) {
            sessionTagsToSet |= transport::Session::kKeepOpen;
        }

        auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx->getClient());
        bool seenIsMaster = clientMetadataIsMasterState.hasSeenIsMaster();

        if (!seenIsMaster) {
            clientMetadataIsMasterState.setSeenIsMaster();
        }

        BSONElement element = cmdObj[kMetadataDocumentName];
        if (!element.eoo()) {
            if (seenIsMaster) {
                uasserted(ErrorCodes::ClientMetadataCannotBeMutated,
                          "The client metadata document may only be sent in the first isMaster");
            }

            auto parsedClientMetadata = uassertStatusOK(ClientMetadata::parse(element));

            invariant(parsedClientMetadata);

            parsedClientMetadata->logClientMetadata(opCtx->getClient());

            clientMetadataIsMasterState.setClientMetadata(opCtx->getClient(),
                                                          std::move(parsedClientMetadata));
        }

        if (!seenIsMaster) {
            auto sniName = opCtx->getClient()->getSniNameForSession();
            SplitHorizon::setParameters(opCtx->getClient(), std::move(sniName));
        }

        // Parse the optional 'internalClient' field. This is provided by incoming connections from
        // mongod and mongos.
        auto internalClientElement = cmdObj["internalClient"];
        if (internalClientElement) {
            sessionTagsToSet |= transport::Session::kInternalClient;
            sessionTagsToUnset |= transport::Session::kExternalClientKeepOpen;

            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "'internalClient' must be of type Object, but was of type "
                                  << typeName(internalClientElement.type()),
                    internalClientElement.type() == BSONType::Object);

            bool foundMaxWireVersion = false;
            for (auto&& elem : internalClientElement.Obj()) {
                auto fieldName = elem.fieldNameStringData();
                if (fieldName == "minWireVersion") {
                    // We do not currently use 'internalClient.minWireVersion'.
                    continue;
                } else if (fieldName == "maxWireVersion") {
                    foundMaxWireVersion = true;

                    uassert(ErrorCodes::TypeMismatch,
                            str::stream() << "'maxWireVersion' field of 'internalClient' must be "
                                             "of type int, but was of type "
                                          << typeName(elem.type()),
                            elem.type() == BSONType::NumberInt);

                    // All incoming connections from mongod/mongos of earlier versions should be
                    // closed if the featureCompatibilityVersion is bumped to 3.6.
                    if (elem.numberInt() >=
                        WireSpec::instance().get()->incomingInternalClient.maxWireVersion) {
                        sessionTagsToSet |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    } else {
                        sessionTagsToUnset |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    }
                } else {
                    uasserted(ErrorCodes::BadValue,
                              str::stream() << "Unrecognized field of 'internalClient': '"
                                            << fieldName << "'");
                }
            }

            uassert(ErrorCodes::BadValue,
                    "Missing required field 'maxWireVersion' of 'internalClient'",
                    foundMaxWireVersion);
        } else {
            sessionTagsToUnset |= (transport::Session::kInternalClient |
                                   transport::Session::kLatestVersionInternalClientKeepOpen);
            sessionTagsToSet |= transport::Session::kExternalClientKeepOpen;
        }

        auto session = opCtx->getClient()->session();
        if (session) {
            session->mutateTags(
                [sessionTagsToSet, sessionTagsToUnset](transport::Session::TagMask originalTags) {
                    // After a mongos sends the initial "isMaster" command with its mongos client
                    // information, it sometimes sends another "isMaster" command that is forwarded
                    // from its client. Once kInternalClient has been set, we assume that any future
                    // "isMaster" commands are forwarded in this manner, and we do not update the
                    // session tags.
                    if ((originalTags & transport::Session::kInternalClient) == 0) {
                        return (originalTags | sessionTagsToSet) & ~sessionTagsToUnset;
                    } else {
                        return originalTags;
                    }
                });
        }

        // If a client is following the awaitable isMaster protocol, maxAwaitTimeMS should be
        // present if and only if topologyVersion is present in the request.
        auto topologyVersionElement = cmdObj["topologyVersion"];
        auto maxAwaitTimeMSField = cmdObj["maxAwaitTimeMS"];
        boost::optional<TopologyVersion> clientTopologyVersion;
        boost::optional<long long> maxAwaitTimeMS;
        if (topologyVersionElement && maxAwaitTimeMSField) {
            clientTopologyVersion = TopologyVersion::parse(IDLParserErrorContext("TopologyVersion"),
                                                           topologyVersionElement.Obj());
            uassert(31372,
                    "topologyVersion must have a non-negative counter",
                    clientTopologyVersion->getCounter() >= 0);

            {
                long long parsedMaxAwaitTimeMS;
                uassertStatusOK(
                    bsonExtractIntegerField(cmdObj, "maxAwaitTimeMS", &parsedMaxAwaitTimeMS));
                maxAwaitTimeMS = parsedMaxAwaitTimeMS;
            }

            uassert(31373, "maxAwaitTimeMS must be a non-negative integer", *maxAwaitTimeMS >= 0);

            LOGV2_DEBUG(23904, 3, "Using maxAwaitTimeMS for awaitable isMaster protocol.");

            // Awaitable isMaster commands have high latency by design.
            opCtx->setShouldIncrementLatencyStats(false);
        } else {
            uassert(31368,
                    (topologyVersionElement
                         ? "A request with a 'topologyVersion' must include 'maxAwaitTimeMS'"
                         : "A request with 'maxAwaitTimeMS' must include a 'topologyVersion'"),
                    !topologyVersionElement && !maxAwaitTimeMSField);
        }

        auto result = replyBuilder->getBodyBuilder();
        auto currentTopologyVersion = appendReplicationInfo(
            opCtx, &result, 0, useLegacyResponseFields(), clientTopologyVersion, maxAwaitTimeMS);

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            const int configServerModeNumber = 2;
            result.append("configsvr", configServerModeNumber);
        }

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        if (auto wireSpec = WireSpec::instance().get(); internalClientElement) {
            result.append("minWireVersion", wireSpec->incomingInternalClient.minWireVersion);
            result.append("maxWireVersion", wireSpec->incomingInternalClient.maxWireVersion);
        } else {
            result.append("minWireVersion", wireSpec->incomingExternalClient.minWireVersion);
            result.append("maxWireVersion", wireSpec->incomingExternalClient.maxWireVersion);
        }

        result.append("readOnly", storageGlobalParams.readOnly);

        const auto& params = ServerParameterSet::getGlobal()->getMap();
        if (auto iter = params.find("automationServiceDescriptor");
            iter != params.end() && iter->second)
            iter->second->append(opCtx, result, "automationServiceDescriptor");

        if (opCtx->getClient()->session()) {
            MessageCompressorManager::forSession(opCtx->getClient()->session())
                .serverNegotiate(cmdObj, &result);
        }

        auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getServiceContext());
        saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, cmdObj, &result);

        if (opCtx->isExhaust()) {
            LOGV2_DEBUG(23905, 3, "Using exhaust for isMaster protocol");

            uassert(51756,
                    "An isMaster request with exhaust must specify 'maxAwaitTimeMS'",
                    maxAwaitTimeMSField);
            invariant(clientTopologyVersion);

            InExhaustIsMaster::get(opCtx->getClient()->session().get())
                ->setInExhaustIsMaster(true /* inExhaustIsMaster */);

            if (clientTopologyVersion->getProcessId() == currentTopologyVersion.getProcessId() &&
                clientTopologyVersion->getCounter() == currentTopologyVersion.getCounter()) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                replyBuilder->setNextInvocation(boost::none);
            } else {
                BSONObjBuilder nextInvocationBuilder;
                for (auto&& elt : cmdObj) {
                    if (elt.fieldNameStringData() == "topologyVersion"_sd) {
                        BSONObjBuilder topologyVersionBuilder(
                            nextInvocationBuilder.subobjStart("topologyVersion"));
                        currentTopologyVersion.serialize(&topologyVersionBuilder);
                    } else {
                        nextInvocationBuilder.append(elt);
                    }
                }
                replyBuilder->setNextInvocation(nextInvocationBuilder.obj());
            }
        }

        handleIsMasterSpeculativeAuth(opCtx, cmdObj, &result);

        return true;
    }

protected:
    CmdHello(const StringData cmdName, const std::initializer_list<StringData>& alias)
        : BasicCommandWithReplyBuilderInterface(cmdName, alias) {}

    virtual bool useLegacyResponseFields() {
        return false;
    }

} cmdhello;

class CmdIsMaster : public CmdHello {
public:
    CmdIsMaster() : CmdHello(kCamelCaseIsMasterString, {kLowerCaseIsMasterString}) {}

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }

protected:
    // Parse the command name, which should be one of the following: hello, isMaster, or
    // ismaster. If the command is "hello", we must attach an "isWritablePrimary" response field
    // instead of "ismaster" and "secondaryDelaySecs" response field instead of "slaveDelay".
    bool useLegacyResponseFields() override {
        return true;
    }

} cmdIsMaster;

OpCounterServerStatusSection replOpCounterServerStatusSection("opcountersRepl", &replOpCounters);

}  // namespace

}  // namespace repl
}  // namespace mongo

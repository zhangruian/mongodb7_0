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

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/speculative_auth.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

namespace mongo {

// Hangs in the beginning of each hello command when set.
MONGO_FAIL_POINT_DEFINE(waitInHello);

MONGO_FAIL_POINT_DEFINE(appendHelloOkToHelloResponse);

namespace {

constexpr auto kHelloString = "hello"_sd;
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;


class CmdHello : public BasicCommandWithReplyBuilderInterface {
public:
    CmdHello() : CmdHello(kHelloString, {}) {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Status information for clients negotiating a connection with this server";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        // No auth required
    }

    bool requiresAuth() const override {
        return false;
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        waitInHello.pauseWhileSet(opCtx);

        auto client = opCtx->getClient();
        ClientMetadata::tryFinalize(client);

        // If a client is following the awaitable hello protocol, maxAwaitTimeMS should be
        // present if and only if topologyVersion is present in the request.
        auto topologyVersionElement = cmdObj["topologyVersion"];
        auto maxAwaitTimeMSField = cmdObj["maxAwaitTimeMS"];
        boost::optional<TopologyVersion> clientTopologyVersion;
        boost::optional<Date_t> deadline;
        if (topologyVersionElement && maxAwaitTimeMSField) {
            clientTopologyVersion = TopologyVersion::parse(IDLParserErrorContext("TopologyVersion"),
                                                           topologyVersionElement.Obj());
            uassert(51758,
                    "topologyVersion must have a non-negative counter",
                    clientTopologyVersion->getCounter() >= 0);

            long long maxAwaitTimeMS;
            uassertStatusOK(bsonExtractIntegerField(cmdObj, "maxAwaitTimeMS", &maxAwaitTimeMS));

            uassert(51759, "maxAwaitTimeMS must be a non-negative integer", maxAwaitTimeMS >= 0);

            deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(maxAwaitTimeMS);

            LOGV2_DEBUG(23871, 3, "Using maxAwaitTimeMS for awaitable hello protocol.");

            // Awaitable hello commands have high latency by design. Ignore them.
            opCtx->setShouldIncrementLatencyStats(false);
        } else {
            uassert(51760,
                    (topologyVersionElement
                         ? "A request with a 'topologyVersion' must include 'maxAwaitTimeMS'"
                         : "A request with 'maxAwaitTimeMS' must include a 'topologyVersion'"),
                    !topologyVersionElement && !maxAwaitTimeMSField);
        }

        auto result = replyBuilder->getBodyBuilder();
        const auto* mongosTopCoord = MongosTopologyCoordinator::get(opCtx);

        auto mongosHelloResponse =
            mongosTopCoord->awaitHelloResponse(opCtx, clientTopologyVersion, deadline);

        mongosHelloResponse->appendToBuilder(&result, useLegacyResponseFields());
        // The hello response always includes a topologyVersion.
        auto currentMongosTopologyVersion = mongosHelloResponse->getTopologyVersion();

        // Try to parse the optional 'helloOk' field. On mongos, if we see this field, we will
        // respond with helloOk: true so the client knows that it can continue to send the hello
        // command to mongos.
        bool helloOk;
        Status status = bsonExtractBooleanField(cmdObj, "helloOk", &helloOk);
        if (status.isOK()) {
            // If the hello request contains a "helloOk" field, set _supportsHello on the Client
            // to the value.
            client->setSupportsHello(helloOk);
            // Attach helloOk: true to the response so that the client knows the server supports
            // the hello command.
            result.append("helloOk", true);
        } else if (status.code() != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }

        if (MONGO_unlikely(appendHelloOkToHelloResponse.shouldFail())) {
            result.append("clientSupportsHello", client->supportsHello());
        }

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        // Mongos tries to keep exactly the same version range of the server for which
        // it is compiled.
        auto wireSpec = WireSpec::instance().get();
        result.append("maxWireVersion", wireSpec->incomingExternalClient.maxWireVersion);
        result.append("minWireVersion", wireSpec->incomingExternalClient.minWireVersion);

        {
            const auto& serverParams = ServerParameterSet::getGlobal()->getMap();
            auto iter = serverParams.find("automationServiceDescriptor");
            if (iter != serverParams.end() && iter->second)
                iter->second->append(opCtx, result, "automationServiceDescriptor");
        }

        MessageCompressorManager::forSession(opCtx->getClient()->session())
            .serverNegotiate(cmdObj, &result);

        auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getServiceContext());
        saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, cmdObj, &result);

        if (opCtx->isExhaust()) {
            LOGV2_DEBUG(23872, 3, "Using exhaust for hello protocol");

            uassert(51763,
                    "A hello/isMaster request with exhaust must specify 'maxAwaitTimeMS'",
                    maxAwaitTimeMSField);
            invariant(clientTopologyVersion);

            InExhaustHello::get(opCtx->getClient()->session().get())
                ->setInExhaust(true /* inExhaust */, getName());

            if (clientTopologyVersion->getProcessId() ==
                    currentMongosTopologyVersion.getProcessId() &&
                clientTopologyVersion->getCounter() == currentMongosTopologyVersion.getCounter()) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                replyBuilder->setNextInvocation(boost::none);
            } else {
                BSONObjBuilder nextInvocationBuilder;
                for (auto&& elt : cmdObj) {
                    if (elt.fieldNameStringData() == "topologyVersion"_sd) {
                        BSONObjBuilder topologyVersionBuilder(
                            nextInvocationBuilder.subobjStart("topologyVersion"));
                        currentMongosTopologyVersion.serialize(&topologyVersionBuilder);
                    } else {
                        nextInvocationBuilder.append(elt);
                    }
                }
                replyBuilder->setNextInvocation(nextInvocationBuilder.obj());
            }
        }

        handleHelloSpeculativeAuth(opCtx, cmdObj, &result);

        return true;
    }

protected:
    CmdHello(const StringData cmdName, const std::initializer_list<StringData>& alias)
        : BasicCommandWithReplyBuilderInterface(cmdName, alias) {}

    virtual bool useLegacyResponseFields() {
        return false;
    }

} hello;

class CmdIsMaster : public CmdHello {

public:
    CmdIsMaster() : CmdHello(kCamelCaseIsMasterString, {kLowerCaseIsMasterString}) {}

protected:
    bool useLegacyResponseFields() override {
        return true;
    }

} isMaster;

}  // namespace
}  // namespace mongo

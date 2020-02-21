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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/async_client.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include "asio.hpp"

namespace mongo {
namespace {

TEST(TransportLayerASIO, HTTPRequestGetsHTTPError) {
    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    asio::io_context ioContext;
    asio::ip::tcp::resolver resolver(ioContext);
    asio::ip::tcp::socket socket(ioContext);

    LOGV2(23028, "Connecting to {server}", "server"_attr = server);
    auto resolverIt = resolver.resolve(server.host(), std::to_string(server.port()));
    asio::connect(socket, resolverIt);

    LOGV2(23029, "Sending HTTP request");
    std::string httpReq = str::stream() << "GET /\r\n"
                                           "Host: "
                                        << server
                                        << "\r\n"
                                           "User-Agent: MongoDB Integration test\r\n"
                                           "Accept: */*";
    asio::write(socket, asio::buffer(httpReq.data(), httpReq.size()));

    LOGV2(23030, "Waiting for response");
    std::array<char, 256> httpRespBuf;
    std::error_code ec;
    auto size = asio::read(socket, asio::buffer(httpRespBuf.data(), httpRespBuf.size()), ec);
    StringData httpResp(httpRespBuf.data(), size);

    LOGV2(23031, "Received response: \"{httpResp}\"", "httpResp"_attr = httpResp);
    ASSERT_TRUE(httpResp.startsWith("HTTP/1.0 200 OK"));

// Why oh why can't ASIO unify their error codes
#ifdef _WIN32
    ASSERT_EQ(ec, asio::error::connection_reset);
#else
    ASSERT_EQ(ec, asio::error::eof);
#endif
}

// This test forces reads and writes to occur one byte at a time, verifying SERVER-34506 (the
// isJustForContinuation optimization works).
//
// Because of the file size limit, it's only an effective check on debug builds (where the future
// implementation checks the length of the future chain).
TEST(TransportLayerASIO, ShortReadsAndWritesWork) {
    const auto assertOK = [](executor::RemoteCommandResponse reply) {
        ASSERT_OK(reply.status);
        ASSERT(reply.data["ok"]) << reply.data;
    };

    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    auto sc = getGlobalServiceContext();
    auto reactor = sc->getTransportLayer()->getReactor(transport::TransportLayer::kNewReactor);

    stdx::thread thread([&] { reactor->run(); });
    const auto threadGuard = makeGuard([&] {
        reactor->stop();
        thread.join();
    });

    AsyncDBClient::Handle handle =
        AsyncDBClient::connect(server, transport::kGlobalSSLMode, sc, reactor, Milliseconds::max())
            .get();

    handle->initWireVersion(__FILE__, nullptr).get();

    FailPointEnableBlock fp("transportLayerASIOshortOpportunisticReadWrite");

    const executor::RemoteCommandRequest ecr{
        server, "admin", BSON("echo" << std::string(1 << 10, 'x')), BSONObj(), nullptr};

    assertOK(handle->runCommandRequest(ecr).get());

    auto client = sc->makeClient(__FILE__);
    auto opCtx = client->makeOperationContext();

    handle->runCommandRequest(ecr, opCtx->getBaton()).get(opCtx.get());
}

TEST(TransportLayerASIO, asyncConnectTimeoutCleansUpSocket) {
    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    auto sc = getGlobalServiceContext();
    auto reactor = sc->getTransportLayer()->getReactor(transport::TransportLayer::kNewReactor);

    stdx::thread thread([&] { reactor->run(); });

    const auto threadGuard = makeGuard([&] {
        reactor->stop();
        thread.join();
    });

    FailPointEnableBlock fp("transportLayerASIOasyncConnectTimesOut");
    auto client =
        AsyncDBClient::connect(server, transport::kGlobalSSLMode, sc, reactor, Milliseconds{500})
            .getNoThrow();
    ASSERT_EQ(client.getStatus(), ErrorCodes::NetworkTimeout);
}

class ExhaustRequestHandlerUtil {
public:
    AsyncDBClient::RemoteCommandCallbackFn&& getExhaustRequestCallbackFn() {
        return std::move(_callbackFn);
    }

    executor::RemoteCommandResponse getReplyObjectWhenReady() {
        stdx::unique_lock<Latch> lk(_mutex);
        _cv.wait(_mutex, [&] { return _replyUpdated; });
        _replyUpdated = false;
        return _reply;
    }

private:
    // holds the server's response once it sent one
    executor::RemoteCommandResponse _reply;
    // set to true once 'reply' has been set. Used to indicate that a new response has been set and
    // should be inspected.
    bool _replyUpdated = false;

    Mutex _mutex = MONGO_MAKE_LATCH();
    stdx::condition_variable _cv;

    // called when a server sends a new isMaster exhaust response. Updates _reply and _replyUpdated.
    AsyncDBClient::RemoteCommandCallbackFn _callbackFn =
        [&](const executor::RemoteCommandResponse& response, bool isMoreToComeSet) {
            {
                stdx::unique_lock<Latch> lk(_mutex);
                _reply = response;
                _replyUpdated = true;
            }

            _cv.notify_all();
        };
};

TEST(TransportLayerASIO, exhaustIsMasterShouldReceiveMultipleReplies) {
    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    auto sc = getGlobalServiceContext();
    auto reactor = sc->getTransportLayer()->getReactor(transport::TransportLayer::kNewReactor);

    stdx::thread thread([&] { reactor->run(); });
    const auto threadGuard = makeGuard([&] {
        reactor->stop();
        thread.join();
    });

    AsyncDBClient::Handle handle =
        AsyncDBClient::connect(server, transport::kGlobalSSLMode, sc, reactor, Milliseconds::max())
            .get();

    handle->initWireVersion(__FILE__, nullptr).get();

    // Send a dummy topologyVersion because the mongod generates this and sends it to the client on
    // the initial handshake.
    auto isMasterRequest = executor::RemoteCommandRequest{
        server,
        "admin",
        BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                        << TopologyVersion(OID::max(), 0).toBSON()),
        BSONObj(),
        nullptr};

    ExhaustRequestHandlerUtil exhaustRequestHandler;
    Future<void> exhaustFuture = handle->runExhaustCommandRequest(
        isMasterRequest, exhaustRequestHandler.getExhaustRequestCallbackFn());

    Date_t prevTime;
    TopologyVersion topologyVersion;
    {
        auto reply = exhaustRequestHandler.getReplyObjectWhenReady();

        ASSERT(!exhaustFuture.isReady());
        ASSERT_OK(reply.status);
        prevTime = reply.data.getField("localTime").Date();
        topologyVersion = TopologyVersion::parse(IDLParserErrorContext("TopologyVersion"),
                                                 reply.data.getField("topologyVersion").Obj());
    }

    {
        auto reply = exhaustRequestHandler.getReplyObjectWhenReady();

        // The moreToCome bit is still set
        ASSERT(!exhaustFuture.isReady());
        ASSERT_OK(reply.status);

        auto replyTime = reply.data.getField("localTime").Date();
        ASSERT_GT(replyTime, prevTime);

        auto replyTopologyVersion = TopologyVersion::parse(
            IDLParserErrorContext("TopologyVersion"), reply.data.getField("topologyVersion").Obj());
        ASSERT_EQ(replyTopologyVersion.getProcessId(), topologyVersion.getProcessId());
        ASSERT_EQ(replyTopologyVersion.getCounter(), topologyVersion.getCounter());
    }

    handle->cancel();
    handle->end();
    auto error = exhaustFuture.getNoThrow();
    // exhaustFuture will resolve with CallbackCanceled unless the socket is already closed, in
    // which case it will resolve with HostUnreachable.
    ASSERT((error == ErrorCodes::CallbackCanceled) || (error == ErrorCodes::HostUnreachable));
}

TEST(TransportLayerASIO, exhaustIsMasterShouldStopOnFailure) {
    const auto assertOK = [](executor::RemoteCommandResponse reply) {
        ASSERT_OK(reply.status);
        ASSERT(reply.data["ok"]) << reply.data;
    };

    auto connectionString = unittest::getFixtureConnectionString();
    auto server = connectionString.getServers().front();

    auto sc = getGlobalServiceContext();
    auto reactor = sc->getTransportLayer()->getReactor(transport::TransportLayer::kNewReactor);

    stdx::thread thread([&] { reactor->run(); });
    const auto threadGuard = makeGuard([&] {
        reactor->stop();
        thread.join();
    });

    AsyncDBClient::Handle isMasterHandle =
        AsyncDBClient::connect(server, transport::kGlobalSSLMode, sc, reactor, Milliseconds::max())
            .get();
    isMasterHandle->initWireVersion(__FILE__, nullptr).get();

    AsyncDBClient::Handle failpointHandle =
        AsyncDBClient::connect(server, transport::kGlobalSSLMode, sc, reactor, Milliseconds::max())
            .get();
    failpointHandle->initWireVersion(__FILE__, nullptr).get();

    // Turn on the failCommand fail point for isMaster
    auto configureFailPointRequest =
        executor::RemoteCommandRequest{
            server,
            "admin",
            BSON("configureFailPoint"
                 << "failCommand"
                 << "mode"
                 << "alwaysOn"
                 << "data"
                 << BSON("errorCode" << ErrorCodes::CommandFailed << "failCommands"
                                     << BSON_ARRAY("isMaster"))),
            BSONObj(),
            nullptr};
    assertOK(failpointHandle->runCommandRequest(configureFailPointRequest).get());

    ON_BLOCK_EXIT([&] {
        auto stopFpRequest = executor::RemoteCommandRequest{server,
                                                            "admin",
                                                            BSON("configureFailPoint"
                                                                 << "failCommand"
                                                                 << "mode"
                                                                 << "off"),
                                                            BSONObj(),
                                                            nullptr};
        assertOK(failpointHandle->runCommandRequest(stopFpRequest).get());
    });

    // Send a dummy topologyVersion because the mongod generates this and sends it to the client on
    // the initial handshake.
    auto isMasterRequest = executor::RemoteCommandRequest{
        server,
        "admin",
        BSON("isMaster" << 1 << "maxAwaitTimeMS" << 1000 << "topologyVersion"
                        << TopologyVersion(OID::max(), 0).toBSON()),
        BSONObj(),
        nullptr};

    ExhaustRequestHandlerUtil exhaustRequestHandler;
    Future<void> exhaustFuture = isMasterHandle->runExhaustCommandRequest(
        isMasterRequest, exhaustRequestHandler.getExhaustRequestCallbackFn());

    {
        auto reply = exhaustRequestHandler.getReplyObjectWhenReady();

        exhaustFuture.get();
        ASSERT_OK(reply.status);
        ASSERT_EQ(reply.data["ok"].Double(), 0.0);
    }
}

}  // namespace
}  // namespace mongo

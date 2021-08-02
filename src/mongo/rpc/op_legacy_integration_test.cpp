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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

std::unique_ptr<DBClientBase> getIntegrationTestConnection() {
    auto swConn = unittest::getFixtureConnectionString().connect("op_legacy_integration_test");
    uassertStatusOK(swConn.getStatus());
    return std::move(swConn.getValue());
}

long getOpCount(BSONObj serverStatus, const char* opName) {
    return serverStatus["opcounters"][opName].Long();
}

long getDeprecatedOpCount(BSONObj serverStatus, const char* opName) {
    auto deprecatedOpcounters = serverStatus["opcounters"]["deprecated"];
    return deprecatedOpcounters ? deprecatedOpcounters[opName].Long() : 0;
}

// Issue a find command request so we can use cursor id from it to test the deprecated getMore
// and killCursors ops.
int64_t getValidCursorIdFromFindCmd(DBClientBase* conn, const char* collName) {
    Message findCmdRequest =
        OpMsgRequest::fromDBAndBody("testOpLegacy", BSON("find" << collName << "batchSize" << 2))
            .serialize();
    Message findCmdReply;
    ASSERT(conn->call(findCmdRequest, findCmdReply));
    BSONObj findCmdReplyBody = OpMsg::parse(findCmdReply).body;
    auto cr = CursorResponse::parseFromBSON(findCmdReplyBody.getOwned());
    ASSERT_OK(cr.getStatus());
    const int64_t cursorId = cr.getValue().getCursorId();
    ASSERT_NE(0, cursorId);

    return cursorId;
}

TEST(OpLegacy, DeprecatedWriteOpsCounters) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "testOpLegacy.DeprecatedWriteOpsCounters";

    // Cache the counters prior to running the deprecated requests.
    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Building parts for the deprecated requests.
    const BSONObj doc1 = fromjson("{a: 1}");
    const BSONObj doc2 = fromjson("{a: 2}");
    const BSONObj insert[2] = {doc1, doc2};
    const BSONObj query = fromjson("{a: {$lt: 42}}");
    const BSONObj update = fromjson("{$set: {b: 2}}");

    // Issue the requests. They are expected to fail but should still be counted.
    Message ignore;
    auto opInsert = makeDeprecatedInsertMessage(ns, insert, 2, 0 /*continue on error*/);
    ASSERT_THROWS(conn->call(opInsert, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opUpdate = makeDeprecatedUpdateMessage(ns, query, update, 0 /*no upsert, no multi*/);
    ASSERT_THROWS(conn->call(opUpdate, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opDelete = makeDeprecatedRemoveMessage(ns, query, 0 /*limit*/);
    ASSERT_THROWS(conn->call(opDelete, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    // Check the opcounters after running the deprecated operations.
    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "insert") + 2,
              getDeprecatedOpCount(serverStatusReply, "insert"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "update") + 1,
              getDeprecatedOpCount(serverStatusReply, "update"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "delete") + 1,
              getDeprecatedOpCount(serverStatusReply, "delete"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "total") + 2 + 1 + 1,
              getDeprecatedOpCount(serverStatusReply, "total"));
}

void assertFailure(const Message response, StringData expectedErr) {
    QueryResult::ConstView qr = response.singleData().view2ptr();
    BufReader responseData(qr.data(), qr.dataLen());
    BSONObj responseBody = responseData.read<BSONObj>();

    ASSERT_FALSE(responseBody["ok"].trueValue()) << responseBody;
    ASSERT_EQ(5739101, responseBody["code"].Int()) << responseBody;
    ASSERT_NE(getErrField(responseBody).checkAndGetStringData().find(expectedErr),
              std::string::npos)
        << responseBody;
}

TEST(OpLegacy, DeprecatedReadOpsCounters) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "testOpLegacy.DeprecatedReadOpsCounters";

    BSONObj insert = fromjson(R"({
        insert: "DeprecatedReadOpsCounters",
        documents: [ {a: 1},{a: 2},{a: 3},{a: 4},{a: 5},{a: 6},{a: 7} ]
    })");
    BSONObj ignoreResponse;
    ASSERT(conn->runCommand("testOpLegacy", insert, ignoreResponse));

    // Cache the counters prior to running the deprecated requests.
    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Issue the deprecated requests. They all should fail one way or another.
    Message opQueryRequest = makeDeprecatedQueryMessage(ns,
                                                        fromjson("{}"),
                                                        2 /*nToReturn*/,
                                                        0 /*nToSkip*/,
                                                        nullptr /*fieldsToReturn*/,
                                                        0 /*queryOptions*/);
    Message opQueryReply;
    ASSERT(conn->call(opQueryRequest, opQueryReply));
    assertFailure(opQueryReply, "OP_QUERY is no longer supported");

    const int64_t cursorId = getValidCursorIdFromFindCmd(conn.get(), "DeprecatedReadOpsCounters");

    Message opGetMoreRequest =
        makeDeprecatedGetMoreMessage(ns, cursorId, 2 /*nToReturn*/, 0 /*flags*/);
    Message opGetMoreReply;
    ASSERT(conn->call(opGetMoreRequest, opGetMoreReply));
    assertFailure(opGetMoreReply, "OP_GET_MORE is no longer supported");

    Message opKillCursorsRequest = makeDeprecatedKillCursorsMessage(cursorId);
    Message opKillCursorsReply;
    ASSERT_THROWS(conn->call(opKillCursorsRequest, opKillCursorsReply),
                  ExceptionForCat<ErrorCategory::NetworkError>);

    // Check the opcounters after running the deprecated operations.
    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "query") + 1,
              getDeprecatedOpCount(serverStatusReply, "query"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "getmore") + 1,
              getDeprecatedOpCount(serverStatusReply, "getmore"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "killcursors") + 1,
              getDeprecatedOpCount(serverStatusReply, "killcursors"));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "total") + 1 + 1 + 1,
              getDeprecatedOpCount(serverStatusReply, "total"));
}

// Check whether the most recent "deprecation" entry in the log matches the given opName and
// severity (if the 'severity' string isn't empty). Return 'false' if no deprecation entries found.
bool wasLogged(DBClientBase* conn, const std::string& opName, const std::string& severity) {
    BSONObj getLogResponse;
    ASSERT(conn->runCommand("admin", fromjson("{getLog: 'global'}"), getLogResponse));

    auto logEntries = getLogResponse["log"].Array();
    for (auto it = logEntries.rbegin(); it != logEntries.rend(); ++it) {
        auto entry = it->String();
        if (entry.find("\"id\":5578800") != std::string::npos) {
            const bool severityMatches = severity.empty() ||
                (entry.find(std::string("\"s\":\"") + severity + "\"") != std::string::npos);
            const bool opNameMatches =
                (entry.find(std::string("\"op\":\"") + opName + "\"") != std::string::npos);
            return severityMatches && opNameMatches;
        }
    }
    return false;
}

void getLastError(DBClientBase* conn) {
    static const auto getLastErrorCommand = fromjson(R"({"getlasterror": 1})");
    BSONObj replyObj;
    conn->runCommand("admin", getLastErrorCommand, replyObj);

    // getLastError command is no longer supported and must always fails.
    auto status = getStatusFromCommandResult(replyObj);
    ASSERT_NOT_OK(status) << replyObj;
    const auto expectedCode = conn->isMongos() ? 5739001 : 5739000;
    ASSERT_EQ(status.code(), expectedCode) << replyObj;
}

void exerciseDeprecatedOps(DBClientBase* conn, const std::string& expectedSeverity) {
    // Build the deprecated requests and the getLog command.
    const std::string ns = "testOpLegacy.exerciseDeprecatedOps";

    // Insert some docs into the collection so even though the legacy write ops are failing we can
    // still test getMore, killCursors and query.
    BSONObj data = fromjson(R"({
        insert: "exerciseDeprecatedOps",
        documents: [ {a: 1},{a: 2},{a: 3},{a: 4},{a: 5},{a: 6},{a: 7} ]
    })");
    BSONObj ignoreResponse;
    ASSERT(conn->runCommand("testOpLegacy", data, ignoreResponse));

    const BSONObj doc1 = fromjson("{a: 1}");
    const BSONObj doc2 = fromjson("{a: 2}");
    const BSONObj insert[2] = {doc1, doc2};
    const BSONObj query = fromjson("{a: {$lt: 42}}");
    const BSONObj update = fromjson("{$set: {b: 2}}");
    auto opInsert = makeDeprecatedInsertMessage(ns, insert, 2, 0 /*continue on error*/);
    auto opUpdate = makeDeprecatedUpdateMessage(ns, query, update, 0 /*no upsert, no multi*/);
    auto opDelete = makeDeprecatedRemoveMessage(ns, query, 0 /*limit*/);
    auto opQuery = makeDeprecatedQueryMessage(
        ns, query, 2 /*nToReturn*/, 0 /*nToSkip*/, nullptr /*fieldsToReturn*/, 0 /*queryOptions*/);
    Message ignore;

    // The first deprecated call after adding a suppression is still logged with elevated severity
    // and after it the suppression kicks in. Any deprecated op can be used to start the suppression
    // period, here we chose getLastError.
    getLastError(conn);

    ASSERT_THROWS(conn->call(opInsert, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "insert", expectedSeverity));

    getLastError(conn);
    ASSERT(wasLogged(conn, "getLastError", expectedSeverity));

    ASSERT_THROWS(conn->call(opUpdate, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "update", expectedSeverity));

    Message replyQuery;
    ASSERT(conn->call(opQuery, replyQuery));
    ASSERT(wasLogged(conn, "query", expectedSeverity));

    int64_t cursorId = getValidCursorIdFromFindCmd(conn, "exerciseDeprecatedOps");

    auto opGetMore = makeDeprecatedGetMoreMessage(ns, cursorId, 2 /*nToReturn*/, 0 /*flags*/);
    Message replyGetMore;
    ASSERT(conn->call(opGetMore, replyGetMore));
    ASSERT(wasLogged(conn, "getmore", expectedSeverity));

    auto opKillCursors = makeDeprecatedKillCursorsMessage(cursorId);
    ASSERT_THROWS(conn->call(opKillCursors, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "killcursors", expectedSeverity));

    ASSERT_THROWS(conn->call(opDelete, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "remove", expectedSeverity));
}

void setDeprecatedWireOpsWarningPeriod(DBClientBase* conn, Seconds timeout) {
    const BSONObj warningTimeout =
        BSON("setParameter" << 1 << "deprecatedWireOpsWarningPeriodInSeconds" << timeout.count());
    BSONObj response;
    ASSERT(conn->runCommand("admin", warningTimeout, response));
}

class DeprecatedWireOpsWarningPeriodScope {
public:
    DeprecatedWireOpsWarningPeriodScope() {
        auto conn = getIntegrationTestConnection();
        BSONObj currentSetting;
        ASSERT(conn->runCommand(
            "admin",
            fromjson("{getParameter: 1, deprecatedWireOpsWarningPeriodInSeconds: 1}"),
            currentSetting));
        timeout = currentSetting["deprecatedWireOpsWarningPeriodInSeconds"].Int();
    }
    ~DeprecatedWireOpsWarningPeriodScope() {
        auto conn = getIntegrationTestConnection();
        setDeprecatedWireOpsWarningPeriod(conn.get(), Seconds{timeout});
    }

private:
    int timeout = 3600;
};

TEST(OpLegacy, DeprecatedOpsLogging) {
    DeprecatedWireOpsWarningPeriodScope timeoutSettingScope;

    auto conn = getIntegrationTestConnection();

    // This test relies on the fact that the suite is run at D2 logging level.
    BSONObj logSettings;
    ASSERT(conn->runCommand(
        "admin", fromjson("{getParameter: 1, logComponentVerbosity: {command: 1}}"), logSettings));
    ASSERT_LTE(2, logSettings["logComponentVerbosity"]["command"]["verbosity"].Int());

    setDeprecatedWireOpsWarningPeriod(conn.get(), Seconds{0} /*timeout*/);
    exerciseDeprecatedOps(conn.get(), "W" /*expectedSeverity*/);

    setDeprecatedWireOpsWarningPeriod(conn.get(), Seconds{3600} /*timeout*/);
    exerciseDeprecatedOps(conn.get(), "D2" /*expectedSeverity*/);
}

TEST(OpLegacy, GenericCommandViaOpQuery) {
    auto conn = getIntegrationTestConnection();

    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Because we cannot link the log entries to the issued commands, limit the search window for
    // the query-related entry in the log by first running a different command (e.g. getLastError).
    getLastError(conn.get());
    ASSERT(wasLogged(conn.get(), "getLastError", ""));

    // The actual command doesn't matter, as long as it's not 'hello' or 'isMaster'.
    auto opQuery = makeDeprecatedQueryMessage("testOpLegacy.$cmd",
                                              serverStatusCmd,
                                              1 /*nToReturn*/,
                                              0 /*nToSkip*/,
                                              nullptr /*fieldsToReturn*/,
                                              0 /*queryOptions*/);
    Message replyQuery;
    ASSERT(conn->call(opQuery, replyQuery));
    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    BufReader data(qr.data(), qr.dataLen());
    BSONObj obj = data.read<BSONObj>();
    auto status = getStatusFromCommandResult(obj);
    ASSERT_EQ(status.code(), ErrorCodes::UnsupportedOpQueryCommand);

    // The logic around log severity for the deprecation logging is tested elsewhere. Here we check
    // that it gets logged at all.
    ASSERT(wasLogged(conn.get(), "query", ""));

    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));
    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "query") + 1,
              getDeprecatedOpCount(serverStatusReply, "query"));
}

// 'hello' and 'isMaster' commands, issued via OP_QUERY protocol, are still fully supported.
void testAllowedCommand(const char* command, ErrorCodes::Error code = ErrorCodes::OK) {
    auto conn = getIntegrationTestConnection();

    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Because we cannot link the log entries to the issued commands, limit the search window for
    // the query-related entry in the log by first running a different command (e.g. getLastError).
    getLastError(conn.get());
    ASSERT(wasLogged(conn.get(), "getLastError", ""));

    auto opQuery = makeDeprecatedQueryMessage("testOpLegacy.$cmd",
                                              fromjson(command),
                                              1 /*nToReturn*/,
                                              0 /*nToSkip*/,
                                              nullptr /*fieldsToReturn*/,
                                              0 /*queryOptions*/);
    Message replyQuery;
    ASSERT(conn->call(opQuery, replyQuery));
    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    BufReader data(qr.data(), qr.dataLen());
    BSONObj obj = data.read<BSONObj>();
    auto status = getStatusFromCommandResult(obj);
    ASSERT_EQ(status.code(), code);

    ASSERT_FALSE(wasLogged(conn.get(), "query", ""));

    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));
    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "query"),
              getDeprecatedOpCount(serverStatusReply, "query"));
}

TEST(OpLegacy, HelloCommandViaOpQuery) {
    testAllowedCommand("{hello: 1}");
}

TEST(OpLegacy, IsMasterCommandViaOpQuery) {
    testAllowedCommand("{isMaster: 1}");
}

TEST(OpLegacy, IsmasterCommandViaOpQuery) {
    testAllowedCommand("{ismaster: 1}");
}

TEST(OpLegacy, IsSelfCommandViaOpQuery) {
    testAllowedCommand("{_isSelf: 1}");
}

TEST(OpLegacy, SaslStartCommandViaOpQuery) {
    // Here we verify that "saslStart" command passes parsing since the request is actually
    // an invalid authentication request which is capture from a log. The AuthenticationFailed error
    // code means that it passes request parsing.
    testAllowedCommand(R"({
                           saslStart: 1,
                           "mechanism":"SCRAM-SHA-256",
                           "options":{"skipEmptyExchange":true},
                           "payload":{
                               "$binary":{
                                   "base64":"biwsbj1fX3N5c3RlbSxyPUlyNDVmQm1WNWNuUXJSS3FhdU9JUERCTUhkV2NrK01i",
                                   "subType":"0"
                               }
                           }
                       })",
                       ErrorCodes::AuthenticationFailed);
}

TEST(OpLegacy, SaslContinueCommandViaOpQuery) {
    // Here we verify that "saslContinue" command passes parsing since the request is actually
    // an invalid authentication request which is captured from a log. The ProtocolError error code
    // means that it passes request parsing.
    testAllowedCommand(R"({
                           saslContinue: 1,
                           "payload":{
                               "$binary":{
                                   "base64":"Yz1iaXdzLHI9SXI0NWZCbVY1Y25RclJLcWF1T0lQREJNSGRXY2srTWJSNE81SnJrcnV4anorRDl2WXkrKzlnNlhBVHFCV0pMbSxwPUJTV3puZnNjcG8rYVhnc1YyT2xEa2NFSjF5NW9rM2xWSWQybjc4NlJ5MTQ9",
                                   "subType":"0"
                               }
                           },
                           "conversationId":1
                       })",
                       ErrorCodes::ProtocolError);
}

}  // namespace
}  // namespace mongo

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

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/dbmessage.h"
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

bool isStandaloneMongod(DBClientBase* conn) {
    return !conn->isReplicaSetMember() && !conn->isMongos();
}

TEST(OpLegacy, DeprecatedWriteOpsCounters) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "test.DeprecatedWriteOpsCounters";

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
    auto opInsert = makeInsertMessage(ns, insert, 2, 0 /*continue on error*/);
    ASSERT_THROWS(conn->call(opInsert, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opUpdate = makeUpdateMessage(ns, query, update, 0 /*no upsert, no multi*/);
    ASSERT_THROWS(conn->call(opUpdate, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opDelete = makeRemoveMessage(ns, query, 0 /*limit*/);
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

TEST(OpLegacy, DeprecatedReadOpsCounters) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "test.DeprecatedReadOpsCounters";

    BSONObj insert = fromjson(R"({
        insert: "DeprecatedReadOpsCounters",
        documents: [ {a: 1},{a: 2},{a: 3},{a: 4},{a: 5},{a: 6},{a: 7} ]
    })");
    BSONObj ignoreResponse;
    ASSERT(conn->runCommand("test", insert, ignoreResponse));

    // Cache the counters prior to running the deprecated requests.
    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Issue the deprecated requests.
    const BSONObj query = fromjson("{a: {$lt: 42}}");

    auto opQuery = makeQueryMessage(
        ns, query, 2 /*nToReturn*/, 0 /*nToSkip*/, nullptr /*fieldsToReturn*/, 0 /*queryOptions*/);
    Message replyQuery;
    ASSERT(conn->call(opQuery, replyQuery));
    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    int64_t cursorId = qr.getCursorId();
    ASSERT_NE(0, cursorId);

    auto opGetMore = makeGetMoreMessage(ns, cursorId, 2 /*nToReturn*/, 0 /*flags*/);
    Message replyGetMore;
    ASSERT(conn->call(opGetMore, replyGetMore));

    auto opKillCursors = makeKillCursorsMessage(cursorId);
    Message ignore;
    ASSERT_THROWS(conn->call(opKillCursors, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    // Check the opcounters after running the deprecated operations.
    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "query") + 1,
              getDeprecatedOpCount(serverStatusReply, "query"));
    if (isStandaloneMongod(conn.get())) {
        ASSERT_EQ(getOpCount(serverStatusReplyPrior, "query") + 1,
                  getOpCount(serverStatusReply, "query"));
    }

    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "getmore") + 1,
              getDeprecatedOpCount(serverStatusReply, "getmore"));
    if (isStandaloneMongod(conn.get())) {
        ASSERT_EQ(getOpCount(serverStatusReplyPrior, "getmore") + 1,
                  getOpCount(serverStatusReply, "getmore"));
    }

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

std::string getLastError(DBClientBase* conn) {
    BSONObj info;
    BSONObjBuilder b;
    b.append("getlasterror", 1);
    conn->runCommand("admin", b.obj(), info);

    if (info["ok"].trueValue()) {
        BSONElement e = info["err"];
        if (e.eoo())
            return "";
        if (e.type() == Object)
            return e.toString();
        return e.str();
    } else {
        // command failure
        BSONElement e = info["errmsg"];
        if (e.eoo())
            return "";
        if (e.type() == Object)
            return "getLastError command failed: " + e.toString();
        return "getLastError command failed: " + e.str();
    }
}

void exerciseDeprecatedOps(DBClientBase* conn, const std::string& expectedSeverity) {
    // Build the deprecated requests and the getLog command.
    const std::string ns = "test.exerciseDeprecatedOps";

    // Insert some docs into the collection so even though the legacy write ops are failing we can
    // still test getMore, killCursors and query.
    BSONObj data = fromjson(R"({
        insert: "exerciseDeprecatedOps",
        documents: [ {a: 1},{a: 2},{a: 3},{a: 4},{a: 5},{a: 6},{a: 7} ]
    })");
    BSONObj ignoreResponse;
    ASSERT(conn->runCommand("test", data, ignoreResponse));

    const BSONObj doc1 = fromjson("{a: 1}");
    const BSONObj doc2 = fromjson("{a: 2}");
    const BSONObj insert[2] = {doc1, doc2};
    const BSONObj query = fromjson("{a: {$lt: 42}}");
    const BSONObj update = fromjson("{$set: {b: 2}}");
    auto opInsert = makeInsertMessage(ns, insert, 2, 0 /*continue on error*/);
    auto opUpdate = makeUpdateMessage(ns, query, update, 0 /*no upsert, no multi*/);
    auto opDelete = makeRemoveMessage(ns, query, 0 /*limit*/);
    auto opQuery = makeQueryMessage(
        ns, query, 2 /*nToReturn*/, 0 /*nToSkip*/, nullptr /*fieldsToReturn*/, 0 /*queryOptions*/);
    Message ignore;

    // The first deprecated call after adding a suppression is still logged with elevated severity
    // and after it the suppression kicks in. Any deprecated op can be used to start the suppression
    // period, here we chose getLastError.
    ASSERT_EQ("", getLastError(conn));

    ASSERT_THROWS(conn->call(opInsert, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "insert", expectedSeverity));

    ASSERT_EQ("", getLastError(conn));
    ASSERT(wasLogged(conn, "getLastError", expectedSeverity));

    ASSERT_THROWS(conn->call(opUpdate, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
    ASSERT(wasLogged(conn, "update", expectedSeverity));

    Message replyQuery;
    ASSERT(conn->call(opQuery, replyQuery));
    ASSERT(wasLogged(conn, "query", expectedSeverity));

    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    int64_t cursorId = qr.getCursorId();
    ASSERT_NE(0, cursorId);
    auto opGetMore = makeGetMoreMessage(ns, cursorId, 2 /*nToReturn*/, 0 /*flags*/);
    Message replyGetMore;
    ASSERT(conn->call(opGetMore, replyGetMore));
    ASSERT(wasLogged(conn, "getmore", expectedSeverity));

    auto opKillCursors = makeKillCursorsMessage(cursorId);
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
    ASSERT_EQ("", getLastError(conn.get()));  // will start failing soon but will still log
    ASSERT(wasLogged(conn.get(), "getLastError", ""));

    // The actual command doesn't matter, as long as it's not 'hello' or 'isMaster'.
    auto opQuery = makeQueryMessage("test.$cmd",
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
    ASSERT_OK(getStatusFromCommandResult(obj));  // will fail after we remove the support for $cmd

    // The logic around log severity for the deprecation logging is tested elsewhere. Here we check
    // that it gets logged at all.
    ASSERT(wasLogged(conn.get(), "query", ""));

    BSONObj serverStatusReply;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReply));
    ASSERT_EQ(getDeprecatedOpCount(serverStatusReplyPrior, "query") + 1,
              getDeprecatedOpCount(serverStatusReply, "query"));
}

// 'hello' and 'isMaster' commands, issued via OP_QUERY protocol, are still fully supported.
void testAllowedCommand(const char* command) {
    auto conn = getIntegrationTestConnection();

    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatusReplyPrior;
    ASSERT(conn->runCommand("admin", serverStatusCmd, serverStatusReplyPrior));

    // Because we cannot link the log entries to the issued commands, limit the search window for
    // the query-related entry in the log by first running a different command (e.g. getLastError).
    ASSERT_EQ("", getLastError(conn.get()));  // will start failing soon but will still log
    ASSERT(wasLogged(conn.get(), "getLastError", ""));

    auto opQuery = makeQueryMessage("test.$cmd",
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
    ASSERT_OK(getStatusFromCommandResult(obj));

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

}  // namespace
}  // namespace mongo

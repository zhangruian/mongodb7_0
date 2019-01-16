
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_driver.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/transaction_coordinator_futures_util.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForParticipantListWriteConcern);
MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForDecisionWriteConcern);

MONGO_FAIL_POINT_DEFINE(hangBeforeWritingParticipantList);
MONGO_FAIL_POINT_DEFINE(hangBeforeWritingDecision);
MONGO_FAIL_POINT_DEFINE(hangBeforeDeletingCoordinatorDoc);

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

using PrepareVote = txn::PrepareVote;
using PrepareResponse = txn::PrepareResponse;
using CommitDecision = txn::CommitDecision;

const WriteConcernOptions kInternalMajorityNoSnapshotWriteConcern(
    WriteConcernOptions::kInternalMajorityNoSnapshot,
    WriteConcernOptions::SyncMode::UNSET,
    WriteConcernOptions::kNoTimeout);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

const ReadPreferenceSetting kPrimaryReadPreference{ReadPreference::PrimaryOnly};

bool isRetryableError(ErrorCodes::Error code) {
    // TODO (SERVER-37880): Consider using RemoteCommandRetryScheduler.
    return std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                     RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                     code) != RemoteCommandRetryScheduler::kAllRetriableErrors.end() ||
        code == ErrorCodes::NetworkInterfaceExceededTimeLimit;
}

BSONArray buildParticipantListMatchesConditions(const std::vector<ShardId>& participantList) {
    BSONArrayBuilder barr;
    for (const auto& participant : participantList) {
        barr.append(participant.toString());
    }

    const long long participantListLength = participantList.size();
    BSONObj participantListHasSize = BSON(TransactionCoordinatorDocument::kParticipantsFieldName
                                          << BSON("$size" << participantListLength));

    BSONObj participantListContains =
        BSON(TransactionCoordinatorDocument::kParticipantsFieldName << BSON("$all" << barr.arr()));

    return BSON_ARRAY(participantListContains << participantListHasSize);
}

std::string buildParticipantListString(const std::vector<ShardId>& participantList) {
    StringBuilder ss;
    ss << "[";
    for (const auto& participant : participantList) {
        ss << participant << " ";
    }
    ss << "]";
    return ss.str();
}

}  // namespace

TransactionCoordinatorDriver::TransactionCoordinatorDriver(ServiceContext* service)
    : _scheduler(service) {}

TransactionCoordinatorDriver::~TransactionCoordinatorDriver() = default;

namespace {
void persistParticipantListBlocking(OperationContext* opCtx,
                                    const LogicalSessionId& lsid,
                                    TxnNumber txnNumber,
                                    const std::vector<ShardId>& participantList) {
    LOG(0) << "Going to write participant list for lsid: " << lsid.toBSON()
           << ", txnNumber: " << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWritingParticipantList)) {
        LOG(0) << "Hit hangBeforeWritingParticipantList failpoint";
    }
    MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeWritingParticipantList);

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::Update updateOp(NamespaceString::kTransactionCoordinatorsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;

            // Ensure that the document for the (lsid, txnNumber) either has no participant list or
            // has the same participant list. The document may have the same participant list if an
            // earlier attempt to write the participant list failed waiting for writeConcern.
            BSONObj noParticipantList = BSON(TransactionCoordinatorDocument::kParticipantsFieldName
                                             << BSON("$exists" << false));
            BSONObj sameParticipantList =
                BSON("$and" << buildParticipantListMatchesConditions(participantList));
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << "$or"
                            << BSON_ARRAY(noParticipantList << sameParticipantList)));

            // Update with participant list.
            TransactionCoordinatorDocument doc;
            doc.setId(std::move(sessionInfo));
            doc.setParticipants(std::move(participantList));
            entry.setU(doc.toBSON());

            entry.setUpsert(true);
            return entry;
        }()});
        return updateOp.serialize({});
    }());

    const auto upsertStatus = getStatusFromWriteCommandReply(commandResponse->getCommandReply());

    // Convert a DuplicateKey error to an anonymous error.
    if (upsertStatus.code() == ErrorCodes::DuplicateKey) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51025,
                  str::stream() << "While attempting to write participant list "
                                << buildParticipantListString(participantList)
                                << " for lsid "
                                << lsid.toBSON()
                                << " and txnNumber "
                                << txnNumber
                                << ", found document for the (lsid, txnNumber) with a different "
                                   "participant list. Current document for the (lsid, txnNumber): "
                                << doc);
    }

    // Throw any other error.
    uassertStatusOK(upsertStatus);

    LOG(0) << "Wrote participant list for lsid: " << lsid.toBSON() << ", txnNumber: " << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWaitingForParticipantListWriteConcern)) {
        LOG(0) << "Hit hangBeforeWaitingForParticipantListWriteConcern failpoint";
    }
    MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
        opCtx, hangBeforeWaitingForParticipantListWriteConcern);

    WriteConcernResult unusedWCResult;
    uassertStatusOK(
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            kInternalMajorityNoSnapshotWriteConcern,
                            &unusedWCResult));
}
}  // namespace

Future<void> TransactionCoordinatorDriver::persistParticipantList(
    const LogicalSessionId& lsid, TxnNumber txnNumber, std::vector<ShardId> participantList) {
    return _scheduler.scheduleWork([lsid, txnNumber, participantList](OperationContext* opCtx) {
        persistParticipantListBlocking(opCtx, lsid, txnNumber, participantList);
    });
}

Future<txn::PrepareVoteConsensus> TransactionCoordinatorDriver::sendPrepare(
    const std::vector<ShardId>& participantShards,
    const LogicalSessionId& lsid,
    TxnNumber txnNumber) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::InternalMajorityNoSnapshot));

    std::vector<Future<PrepareResponse>> responses;

    // Send prepare to all participants asynchronously and collect their future responses in a
    // vector of responses.
    for (const auto& participant : participantShards) {
        responses.push_back(sendPrepareToShard(participant, prepareObj));
    }

    // Asynchronously aggregate all prepare responses to find the decision and max prepare timestamp
    // (used for commit), stopping the aggregation and preventing any further retries as soon as an
    // abort decision is received. Return a future containing the result.
    return collect(
        std::move(responses),
        // Initial value
        txn::PrepareVoteConsensus{boost::none, boost::none},
        // Aggregates an incoming response (next) with the existing aggregate value (result)
        [this](txn::PrepareVoteConsensus& result, const PrepareResponse& next) {
            if (!next.vote) {
                LOG(3) << "Transaction coordinator did not receive a response from shard "
                       << next.participantShardId;
                return txn::ShouldStopIteration::kNo;
            }

            switch (*next.vote) {
                case PrepareVote::kAbort:
                    if (result.decision == CommitDecision::kAbort) {
                        LOG(3) << "Ignoring vote to abort from shard " << next.participantShardId
                               << " because a vote to abort has already been received";
                        break;
                    }
                    result.decision = CommitDecision::kAbort;
                    result.maxPrepareTimestamp = boost::none;
                    cancel();
                    break;
                case PrepareVote::kCommit:
                    if (result.decision == CommitDecision::kAbort) {
                        LOG(3) << "Ignoring vote to commit from shard " << next.participantShardId
                               << " because a vote to abort has already been received";
                        break;
                    }

                    result.decision = CommitDecision::kCommit;
                    result.maxPrepareTimestamp = (result.maxPrepareTimestamp)
                        ? std::max(result.maxPrepareTimestamp, next.prepareTimestamp)
                        : next.prepareTimestamp;
                    break;
            }

            return txn::ShouldStopIteration::kNo;
        });
}

namespace {
void persistDecisionBlocking(OperationContext* opCtx,
                             const LogicalSessionId& lsid,
                             TxnNumber txnNumber,
                             const std::vector<ShardId>& participantList,
                             const boost::optional<Timestamp>& commitTimestamp) {
    LOG(0) << "Going to write decision " << (commitTimestamp ? "commit" : "abort")
           << " for lsid: " << lsid.toBSON() << ", txnNumber: " << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWritingDecision)) {
        LOG(0) << "Hit hangBeforeWritingDecision failpoint";
    }
    MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeWritingDecision);

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    const auto commandResponse = client.runCommand([&] {
        write_ops::Update updateOp(NamespaceString::kTransactionCoordinatorsNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;

            // Ensure that the document for the (lsid, txnNumber) has the same participant list and
            // either has no decision or the same decision. The document may have the same decision
            // if an earlier attempt to write the decision failed waiting for writeConcern.
            BSONObj noDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                      << BSON("$exists" << false)
                                      << "commitTimestamp"
                                      << BSON("$exists" << false));
            BSONObj sameDecision;
            if (commitTimestamp) {
                sameDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                    << "commit"
                                    << TransactionCoordinatorDocument::kCommitTimestampFieldName
                                    << *commitTimestamp);
            } else {
                sameDecision = BSON(TransactionCoordinatorDocument::kDecisionFieldName
                                    << "abort"
                                    << TransactionCoordinatorDocument::kCommitTimestampFieldName
                                    << BSON("$exists" << false));
            }
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << "$and"
                            << buildParticipantListMatchesConditions(participantList)
                            << "$or"
                            << BSON_ARRAY(noDecision << sameDecision)));

            // Update with decision.
            TransactionCoordinatorDocument doc;
            doc.setId(sessionInfo);
            doc.setParticipants(std::move(participantList));
            if (commitTimestamp) {
                doc.setDecision("commit"_sd);
                doc.setCommitTimestamp(commitTimestamp);
            } else {
                doc.setDecision("abort"_sd);
            }
            entry.setU(doc.toBSON());

            return entry;
        }()});
        return updateOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    // If no document matched, throw an anonymous error. (The update itself will not have thrown an
    // error, because it's legal for an update to match no documents.)
    if (commandReply.getIntField("n") != 1) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51026,
                  str::stream() << "While attempting to write decision "
                                << (commitTimestamp ? "'commit'" : "'abort'")
                                << " for lsid "
                                << lsid.toBSON()
                                << " and txnNumber "
                                << txnNumber
                                << ", either failed to find document for this (lsid, txnNumber) or "
                                   "document existed with a different participant list, different "
                                   "decision, or different commitTimestamp. Current document for "
                                   "the (lsid, txnNumber): "
                                << doc);
    }

    LOG(0) << "Wrote decision " << (commitTimestamp ? "commit" : "abort")
           << " for lsid: " << lsid.toBSON() << ", txnNumber: " << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeWaitingForDecisionWriteConcern)) {
        LOG(0) << "Hit hangBeforeWaitingForDecisionWriteConcern failpoint";
    }
    MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx,
                                                    hangBeforeWaitingForDecisionWriteConcern);

    WriteConcernResult unusedWCResult;
    uassertStatusOK(
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            kInternalMajorityNoSnapshotWriteConcern,
                            &unusedWCResult));
}
}  // namespace

Future<void> TransactionCoordinatorDriver::persistDecision(
    const LogicalSessionId& lsid,
    TxnNumber txnNumber,
    std::vector<ShardId> participantList,
    const boost::optional<Timestamp>& commitTimestamp) {
    return _scheduler.scheduleWork(
        [lsid, txnNumber, participantList, commitTimestamp](OperationContext* opCtx) {
            persistDecisionBlocking(opCtx, lsid, txnNumber, participantList, commitTimestamp);
        });
}

Future<void> TransactionCoordinatorDriver::sendCommit(const std::vector<ShardId>& participantShards,
                                                      const LogicalSessionId& lsid,
                                                      TxnNumber txnNumber,
                                                      Timestamp commitTimestamp) {
    CommitTransaction commitTransaction;
    commitTransaction.setCommitTimestamp(commitTimestamp);
    commitTransaction.setDbName("admin");
    BSONObj commitObj = commitTransaction.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    std::vector<Future<void>> responses;
    for (const auto& participant : participantShards) {
        responses.push_back(sendDecisionToParticipantShard(participant, commitObj));
    }
    return txn::whenAll(responses);
}

Future<void> TransactionCoordinatorDriver::sendAbort(const std::vector<ShardId>& participantShards,
                                                     const LogicalSessionId& lsid,
                                                     TxnNumber txnNumber) {
    BSONObj abortObj =
        BSON("abortTransaction" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << txnNumber
                                << "autocommit"
                                << false
                                << WriteConcernOptions::kWriteConcernField
                                << WriteConcernOptions::Majority);

    std::vector<Future<void>> responses;
    for (const auto& participant : participantShards) {
        responses.push_back(sendDecisionToParticipantShard(participant, abortObj));
    }
    return txn::whenAll(responses);
}

namespace {
void deleteCoordinatorDocBlocking(OperationContext* opCtx,
                                  const LogicalSessionId& lsid,
                                  TxnNumber txnNumber) {
    LOG(0) << "Going to delete coordinator doc for lsid: " << lsid.toBSON()
           << ", txnNumber: " << txnNumber;

    if (MONGO_FAIL_POINT(hangBeforeDeletingCoordinatorDoc)) {
        LOG(0) << "Hit hangBeforeDeletingCoordinatorDoc failpoint";
    }
    MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangBeforeDeletingCoordinatorDoc);

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    DBDirectClient client(opCtx);

    // Throws if serializing the request or deserializing the response fails.
    auto commandResponse = client.runCommand([&] {
        write_ops::Delete deleteOp(NamespaceString::kTransactionCoordinatorsNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;

            // Ensure the document is only deleted after a decision has been made.
            BSONObj abortDecision =
                BSON(TransactionCoordinatorDocument::kDecisionFieldName << "abort");
            BSONObj commitDecision =
                BSON(TransactionCoordinatorDocument::kDecisionFieldName << "commit");
            entry.setQ(BSON(TransactionCoordinatorDocument::kIdFieldName
                            << sessionInfo.toBSON()
                            << TransactionCoordinatorDocument::kDecisionFieldName
                            << BSON("$exists" << true)));

            entry.setMulti(false);
            return entry;
        }()});
        return deleteOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    // If no document matched, throw an anonymous error. (The delete itself will not have thrown an
    // error, because it's legal for a delete to match no documents.)
    if (commandReply.getIntField("n") != 1) {
        // Attempt to include the document for this (lsid, txnNumber) in the error message, if one
        // exists. Note that this is best-effort: the document may have been deleted or manually
        // changed since the update above ran.
        const auto doc = client.findOne(
            NamespaceString::kTransactionCoordinatorsNamespace.toString(),
            QUERY(TransactionCoordinatorDocument::kIdFieldName << sessionInfo.toBSON()));
        uasserted(51027,
                  str::stream() << "While attempting to delete document for lsid " << lsid.toBSON()
                                << " and txnNumber "
                                << txnNumber
                                << ", either failed to find document for this (lsid, txnNumber) or "
                                   "document existed without a decision. Current document for the "
                                   "(lsid, txnNumber): "
                                << doc);
    }

    LOG(0) << "Deleted coordinator doc for lsid: " << lsid.toBSON() << ", txnNumber: " << txnNumber;
}
}  // namespace

Future<void> TransactionCoordinatorDriver::deleteCoordinatorDoc(const LogicalSessionId& lsid,
                                                                TxnNumber txnNumber) {
    return _scheduler.scheduleWork([lsid, txnNumber](OperationContext* opCtx) {
        deleteCoordinatorDocBlocking(opCtx, lsid, txnNumber);
    });
}

std::vector<TransactionCoordinatorDocument> TransactionCoordinatorDriver::readAllCoordinatorDocs(
    OperationContext* opCtx) {
    std::vector<TransactionCoordinatorDocument> allCoordinatorDocs;

    Query query;
    DBDirectClient client(opCtx);
    auto coordinatorDocsCursor =
        client.query(NamespaceString::kTransactionCoordinatorsNamespace, query);

    while (coordinatorDocsCursor->more()) {
        // TODO (SERVER-38307): Try/catch around parsing the document and skip the document if it
        // fails to parse.
        auto nextDecision = TransactionCoordinatorDocument::parse(IDLParserErrorContext(""),
                                                                  coordinatorDocsCursor->next());
        allCoordinatorDocs.push_back(nextDecision);
    }

    return allCoordinatorDocs;
}

Future<PrepareResponse> TransactionCoordinatorDriver::sendPrepareToShard(
    const ShardId& shardId, const BSONObj& commandObj) {
    return txn::doWhile(
        _scheduler,
        kExponentialBackoff,
        [](StatusWith<PrepareResponse> s) { return isRetryableError(s.getStatus().code()); },
        [ this, shardId, commandObj = commandObj.getOwned() ] {
            return _scheduler.scheduleRemoteCommand(shardId, kPrimaryReadPreference, commandObj)
                .then([ shardId, commandObj = commandObj.getOwned() ](ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    if (status.isOK()) {
                        auto prepareTimestampField = response.data["prepareTimestamp"];
                        if (prepareTimestampField.eoo() ||
                            prepareTimestampField.timestamp().isNull()) {
                            LOG(0) << "Coordinator shard received an OK response to "
                                      "prepareTransaction "
                                      "without a prepareTimestamp from shard "
                                   << shardId << ", which is not expected behavior. "
                                                 "Interpreting the response from "
                                   << shardId << " as a vote to abort";
                            return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
                        }

                        LOG(3) << "Coordinator shard received a vote to commit from shard "
                               << shardId
                               << " with prepareTimestamp: " << prepareTimestampField.timestamp();
                        return PrepareResponse{
                            shardId, PrepareVote::kCommit, prepareTimestampField.timestamp()};
                    }

                    LOG(3) << "Coordinator shard received " << status << " from shard " << shardId
                           << " for " << commandObj;
                    if (ErrorCodes::isVoteAbortError(status.code())) {
                        return PrepareResponse{shardId, PrepareVote::kAbort, boost::none};
                    }

                    uassertStatusOK(status);
                    MONGO_UNREACHABLE;
                })
                .onError<ErrorCodes::ShardNotFound>([shardId](const Status& s) {
                    // ShardNotFound may indicate that the participant shard has been removed (it
                    // could also mean the participant shard was recently added and this node
                    // refreshed its ShardRegistry from a stale config secondary).
                    //
                    // Since this node can't know which is the case, it is safe to pessimistically
                    // treat ShardNotFound as a vote to abort, which is always safe since the node
                    // must then send abort.
                    return Future<PrepareResponse>::makeReady(
                        {shardId, CommitDecision::kAbort, boost::none});
                })
                .onError([this, shardId](const Status& status) {
                    if (_cancelled.loadRelaxed()) {
                        LOG(3) << "Prepare stopped retrying due to retrying being cancelled";
                        return PrepareResponse{shardId, boost::none, boost::none};
                    }

                    if (!isRetryableError(status.code())) {
                        return PrepareResponse{shardId, CommitDecision::kAbort, boost::none};
                    }

                    uassertStatusOK(status);
                    MONGO_UNREACHABLE;
                });
        });
}

Future<void> TransactionCoordinatorDriver::sendDecisionToParticipantShard(
    const ShardId& shardId, const BSONObj& commandObj) {
    return txn::doWhile(
        _scheduler,
        kExponentialBackoff,
        [](const Status& s) { return isRetryableError(s.code()); },
        [ this, shardId, commandObj = commandObj.getOwned() ] {
            return _scheduler.scheduleRemoteCommand(shardId, kPrimaryReadPreference, commandObj)
                .then([ shardId, commandObj = commandObj.getOwned() ](ResponseStatus response) {
                    auto status = getStatusFromCommandResult(response.data);
                    auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);

                    // There must be no writeConcern error in order for us to interpret the command
                    // response.
                    if (!wcStatus.isOK()) {
                        status = wcStatus;
                    }

                    LOG(3) << "Coordinator shard received " << status << " in response to "
                           << commandObj << " from shard " << shardId;

                    if (ErrorCodes::isVoteAbortError(status.code())) {
                        // Interpret voteAbort errors as an ack.
                        status = Status::OK();
                    }

                    return status;
                })
                .onError<ErrorCodes::ShardNotFound>([](const Status& s) {
                    // TODO (SERVER-38918): Unlike for prepare, there is no pessimistic way to
                    // handle ShardNotFound. It's not safe to treat ShardNotFound as an ack, because
                    // this node may have refreshed its ShardRegistry from a stale config secondary.
                    MONGO_UNREACHABLE;
                });
        });
}

void TransactionCoordinatorDriver::cancel() {
    _cancelled.store(true);
}

}  // namespace mongo

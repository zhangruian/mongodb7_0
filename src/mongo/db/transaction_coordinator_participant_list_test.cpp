
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

#include "mongo/platform/basic.h"

#include <deque>

#include "mongo/db/transaction_coordinator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using ParticipantList = TransactionCoordinator::ParticipantList;

const Timestamp dummyTimestamp = Timestamp::min();

TEST(ParticipantList, ReceiveSameParticipantListMultipleTimesSucceeds) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
}

TEST(ParticipantList, ReceiveConflictingParticipantListsNoOverlapThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0002"), ShardId("shard0003")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveConflictingParticipantListsFirstListIsSupersetOfSecondThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0000")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveConflictingParticipantListsFirstListIsSubsetOfSecondThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList(
                           {ShardId("shard0000"), ShardId("shard0001"), ShardId("shard0002")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveVoteAbortFromParticipantNotInListThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000")});
    ASSERT_THROWS_CODE(participantList.recordVoteAbort(ShardId("shard0001")),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveVoteCommitFromParticipantNotInListThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000")});
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0001"), dummyTimestamp),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveParticipantListMissingParticipantThatAlreadyVotedAbortThrows) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0000"));
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0001")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveParticipantListMissingParticipantThatAlreadyVotedCommitThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), dummyTimestamp);
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0001")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantResendsVoteAbortSucceeds) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0001"));
    participantList.recordVoteAbort(ShardId("shard0001"));
}

TEST(ParticipantList, ParticipantResendsVoteCommitSucceeds) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), dummyTimestamp);
    participantList.recordVoteCommit(ShardId("shard0000"), dummyTimestamp);
}

TEST(ParticipantList, ParticipantChangesVoteFromAbortToCommitThrows) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0000"));
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0000"), dummyTimestamp),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantChangesVoteFromCommitToAbortThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), dummyTimestamp);
    ASSERT_THROWS_CODE(participantList.recordVoteAbort(ShardId("shard0000")),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantChangesPrepareTimestampThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), Timestamp::min());
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0000"), Timestamp::max()),
                       AssertionException,
                       ErrorCodes::InternalError);
}

}  // namespace mongo

/**
 *    Copyright (C) 2018 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <deque>

#include "mongo/db/transaction_coordinator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::deque;
using std::map;

using StateMachine = TransactionCoordinator::StateMachine;
using State = StateMachine::State;
using Event = StateMachine::Event;
using Action = StateMachine::Action;

using Schedule = deque<Event>;

void runSchedule(StateMachine& coordinator, Schedule schedule) {
    while (!schedule.empty()) {
        stdx::mutex mutex;
        stdx::unique_lock<stdx::mutex> lk(mutex);

        coordinator.onEvent(std::move(lk), schedule.front());
        schedule.pop_front();
    }
}

void expectScheduleSucceeds(Schedule schedule, State expectedEndState) {
    StateMachine coordinator;
    runSchedule(coordinator, schedule);
    ASSERT_EQ(expectedEndState, coordinator.state());
}

void expectScheduleThrows(Schedule schedule) {
    StateMachine coordinator;
    ASSERT_THROWS(runSchedule(coordinator, schedule), AssertionException);
    ASSERT_EQ(State::kBroken, coordinator.state());
}

TEST(CoordinatorStateMachine, AbortSucceeds) {
    expectScheduleSucceeds({Event::kRecvVoteAbort}, State::kAborted);
    expectScheduleSucceeds({Event::kRecvVoteAbort, Event::kRecvVoteAbort}, State::kAborted);
}

TEST(CoordinatorStateMachine, CommitSucceeds) {
    expectScheduleSucceeds(
        {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit, Event::kRecvFinalCommitAck},
        State::kCommitted);
}

TEST(CoordinatorStateMachine, RecvFinalVoteCommitAndRecvVoteAbortThrows) {
    expectScheduleThrows({Event::kRecvVoteAbort, Event::kRecvFinalVoteCommit});
    expectScheduleThrows(
        {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit, Event::kRecvVoteAbort});
}

TEST(CoordinatorStateMachine, WaitForTransitionToOnlyTerminalStatesReturnsCorrectStateOnAbort) {
    StateMachine coordinator;
    auto future = coordinator.waitForTransitionTo({State::kCommitted, State::kAborted});
    runSchedule(coordinator, {Event::kRecvVoteAbort});
    ASSERT_EQ(future.get(), State::kAborted);
}

TEST(CoordinatorStateMachine, WaitForTransitionToStatesThatHaventBeenReachedReturnsNotReadyFuture) {
    StateMachine coordinator;
    auto future = coordinator.waitForTransitionTo({State::kCommitted, State::kAborted});
    ASSERT_FALSE(future.isReady());
    // We need to abort here because we require that all promises are triggered prior to coordinator
    // destruction.
    runSchedule(coordinator, {Event::kRecvVoteAbort});
}

TEST(CoordinatorStateMachine, WaitForTransitionToOnlyTerminalStatesReturnsCorrectStateOnCommit) {
    StateMachine coordinator;
    auto future = coordinator.waitForTransitionTo({State::kCommitted, State::kAborted});
    runSchedule(
        coordinator,
        {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit, Event::kRecvFinalCommitAck});
    ASSERT_EQ(future.get(), State::kCommitted);
}

TEST(CoordinatorStateMachine,
     WaitForTransitionToSingleStateAfterStateHasBeenReachedReturnsCorrectState) {
    StateMachine coordinator;
    runSchedule(coordinator, {Event::kRecvParticipantList});
    auto future = coordinator.waitForTransitionTo(
        {State::kWaitingForVotes, State::kCommitted, State::kAborted});
    ASSERT_EQ(future.get(), TransactionCoordinator::StateMachine::State::kWaitingForVotes);
}

TEST(CoordinatorStateMachine, WaitForTransitionToMultipleStatesReturnsFirstStateToBeHit) {
    StateMachine coordinator;
    auto future = coordinator.waitForTransitionTo({State::kWaitingForCommitAcks,
                                                   State::kWaitingForVotes,
                                                   State::kCommitted,
                                                   State::kAborted});

    runSchedule(coordinator, {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit});

    ASSERT_EQ(future.get(), TransactionCoordinator::StateMachine::State::kWaitingForVotes);
}

TEST(CoordinatorStateMachine,
     TwoWaitForTransitionCallsToDifferentSetsOfStatesReturnsCorrectStateForEach) {
    StateMachine coordinator;
    auto future1 = coordinator.waitForTransitionTo(
        {State::kWaitingForVotes, State::kCommitted, State::kAborted});
    auto future2 = coordinator.waitForTransitionTo(
        {State::kWaitingForCommitAcks, State::kCommitted, State::kAborted});
    runSchedule(coordinator, {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit});

    ASSERT_EQ(future1.get(), TransactionCoordinator::StateMachine::State::kWaitingForVotes);
    ASSERT_EQ(future2.get(), TransactionCoordinator::StateMachine::State::kWaitingForCommitAcks);
}

TEST(CoordinatorStateMachine,
     SeveralWaitForTransitionCallsToDifferentSetsOfStatesReturnsCorrectStateForEach) {

    StateMachine coordinator;
    auto futures1 = {coordinator.waitForTransitionTo(
                         {State::kWaitingForVotes, State::kCommitted, State::kAborted}),
                     coordinator.waitForTransitionTo(
                         {State::kWaitingForVotes, State::kCommitted, State::kAborted}),
                     coordinator.waitForTransitionTo(
                         {State::kWaitingForVotes, State::kCommitted, State::kAborted})};
    auto futures2 = {coordinator.waitForTransitionTo(
                         {State::kWaitingForCommitAcks, State::kCommitted, State::kAborted}),
                     coordinator.waitForTransitionTo(
                         {State::kWaitingForCommitAcks, State::kCommitted, State::kAborted}),
                     coordinator.waitForTransitionTo(
                         {State::kWaitingForCommitAcks, State::kCommitted, State::kAborted})};

    runSchedule(coordinator, {Event::kRecvParticipantList, Event::kRecvFinalVoteCommit});

    for (auto& future1 : futures1) {
        ASSERT_EQ(future1.get(), TransactionCoordinator::StateMachine::State::kWaitingForVotes);
    }
    for (auto& future2 : futures2) {
        ASSERT_EQ(future2.get(),
                  TransactionCoordinator::StateMachine::State::kWaitingForCommitAcks);
    }
}

DEATH_TEST(CoordinatorStateMachine,
           MustNotHaveOutstandingPromisesWhenDestroyed,
           "Invariant failure") {
    StateMachine coordinator;
    auto future = coordinator.waitForTransitionTo(
        {State::kWaitingForVotes, State::kCommitted, State::kAborted});
}

}  // namespace mongo

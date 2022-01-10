/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/health_observer.h"

#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/db/process_health/health_observer_mock.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/timer.h"

namespace mongo {

namespace process_health {

// Using the common fault manager test suite.
using test::FaultManagerTest;

namespace {
// Tests that the mock observer is registered properly.
// This test requires that actual production health observers (e.g. Ldap)
// are not linked with this test, otherwise the count of observers returned
// by the instantiate method below will be greater than expected.
TEST_F(FaultManagerTest, Registration) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    auto allObservers = HealthObserverRegistration::instantiateAllObservers(svcCtx());
    ASSERT_EQ(1, allObservers.size());
    ASSERT_EQ(FaultFacetType::kMock1, allObservers[0]->getType());
}

TEST_F(FaultManagerTest, InitialHealthCheckDoesNotRunIfFeatureFlagNotEnabled) {
    resetManager();
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", false};

    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    static_cast<void>(manager().schedulePeriodicHealthCheckThreadTest());

    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.
    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);
}

TEST_F(FaultManagerTest, Stats) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    auto observer = manager().getHealthObserversTest()[0];
    manager().healthCheckTest(observer, CancellationToken::uncancelable());

    assertSoon([this] { return static_cast<bool>(manager().currentFault()); });
    assertSoon([&observer] { return !observer->getStats().currentlyRunningHealthCheck; });

    auto stats = observer->getStats();
    ASSERT_TRUE(manager().getConfig().isHealthObserverEnabled(observer->getType()));
    ASSERT_EQ(stats.lastTimeCheckStarted, clockSource().now());
    ASSERT_EQ(stats.lastTimeCheckCompleted, stats.lastTimeCheckStarted);
    ASSERT_GTE(stats.completedChecksCount, 1);
    ASSERT_GTE(stats.completedChecksWithFaultCount, 1);

    // To complete initial health check.
    manager().acceptTest(HealthCheckStatus(faultFacetType));

    advanceTime(Milliseconds(200));
    auto prevStats = stats;
    do {
        manager().healthCheckTest(observer, CancellationToken::uncancelable());
        sleepmillis(1);
        observer = manager().getHealthObserversTest()[0];
        stats = observer->getStats();
    } while (stats.completedChecksCount <= prevStats.completedChecksCount);

    ASSERT_GT(stats.lastTimeCheckStarted, prevStats.lastTimeCheckStarted);
    ASSERT_GT(stats.lastTimeCheckCompleted, prevStats.lastTimeCheckCompleted);
    ASSERT_GTE(stats.completedChecksCount, 2);
    ASSERT_GTE(stats.completedChecksWithFaultCount, 2);
}

TEST_F(FaultManagerTest, ProgressMonitorCheck) {
    AtomicWord<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1, [&shouldBlock] {
        while (shouldBlock.load()) {
            sleepFor(Milliseconds(1));
        }
        return Severity::kFailure;
    });

    // Health check should get stuck here.
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    auto observer = manager().getHealthObserversTest()[0];
    manager().healthCheckTest(observer, CancellationToken::uncancelable());

    // Verify that the 'crash callback' is invoked after timeout.
    bool crashTriggered = false;
    std::function<void(std::string cause)> crashCb = [&crashTriggered](std::string) {
        crashTriggered = true;
    };
    manager().progressMonitorCheckTest(crashCb);
    // The progress check passed because the simulated time did not advance.
    ASSERT_FALSE(crashTriggered);
    advanceTime(manager().getConfig().getPeriodicLivenessDeadline() + Seconds(1));
    manager().progressMonitorCheckTest(crashCb);
    // The progress check simulated a crash.
    ASSERT_TRUE(crashTriggered);
    shouldBlock.store(false);
    resetManager();  // Before fields above go out of scope.
}

TEST_F(FaultManagerTest, HealthCheckRunsPeriodically) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type"
                                         << "test"
                                         << "interval" << 1)))};
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    AtomicWord<Severity> severity{Severity::kOk};
    registerMockHealthObserver(faultFacetType, [&severity] { return severity.load(); });

    assertSoon([this] { return (manager().getFaultState() == FaultState::kStartupCheck); });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this] { return (manager().getFaultState() == FaultState::kOk); });

    severity.store(Severity::kFailure);
    assertSoon([this] { return (manager().getFaultState() == FaultState::kTransientFault); });
    resetManager();  // Before fields above go out of scope.
}

TEST_F(FaultManagerTest, PeriodicHealthCheckOnErrorMakesBadHealthStatus) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;

    registerMockHealthObserver(faultFacetType, [] {
        uassert(ErrorCodes::InternalError, "test exception", false);
        return Severity::kFailure;
    });

    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this] {
        return manager().currentFault() && manager().getFaultState() == FaultState::kStartupCheck;
    });
}

TEST_F(FaultManagerTest,
       DeadlineFutureCausesTransientFaultWhenObserverBlocksAndGetsResolvedWhenObserverUnblocked) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type"
                                         << "test"
                                         << "interval" << 1)))};
    RAIIServerParameterControllerForTest _flagController{"featureFlagHealthMonitoring", true};
    RAIIServerParameterControllerForTest _serverParamController{"activeFaultDurationSecs", 5};

    AtomicWord<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1,
                               [&shouldBlock] {
                                   while (shouldBlock.load()) {
                                       sleepFor(Milliseconds(1));
                                   }
                                   return Severity::kOk;
                               },
                               Milliseconds(100));

    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this] {
        return manager().currentFault() && manager().getFaultState() == FaultState::kStartupCheck;
    });

    shouldBlock.store(false);

    assertSoon([this] { return manager().getFaultState() == FaultState::kOk; });

    resetManager();  // Before fields above go out of scope.
}

}  // namespace
}  // namespace process_health
}  // namespace mongo

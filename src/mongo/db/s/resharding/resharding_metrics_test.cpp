/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

constexpr auto kOpTimeRemaining = "remainingOperationTimeEstimated"_sd;

class ReshardingMetricsTest : public ServiceContextTest {
public:
    enum OpReportType {
        CumulativeReport,
        CurrentOpReportDonorRole,
        CurrentOpReportRecipientRole,
        CurrentOpReportCoordinatorRole
    };

    void setUp() override {
        auto clockSource = std::make_unique<ClockSourceMock>();
        _clockSource = clockSource.get();
        getGlobalServiceContext()->setFastClockSource(std::move(clockSource));
    }

    auto getMetrics() {
        return ReshardingMetrics::get(getGlobalServiceContext());
    }

    // Timer step in milliseconds
    static constexpr auto kTimerStep = 100;

    void advanceTime(Milliseconds step = Milliseconds{kTimerStep}) {
        _clockSource->advance(step);
    }

    auto getReport(OpReportType reportType) {
        BSONObjBuilder bob;
        if (reportType == OpReportType::CumulativeReport) {
            getMetrics()->serializeCumulativeOpMetrics(&bob);
        } else if (reportType == OpReportType::CurrentOpReportDonorRole) {
            getMetrics()->serializeCurrentOpMetrics(
                &bob, ReshardingMetrics::ReporterOptions::Role::kDonor);
        } else if (reportType == OpReportType::CurrentOpReportRecipientRole) {
            getMetrics()->serializeCurrentOpMetrics(
                &bob, ReshardingMetrics::ReporterOptions::Role::kRecipient);
        } else {
            getMetrics()->serializeCurrentOpMetrics(
                &bob, ReshardingMetrics::ReporterOptions::Role::kCoordinator);
        }
        return bob.obj();
    }

    void checkMetrics(std::string tag, int expectedValue, OpReportType reportType) {
        const auto report = getReport(reportType);
        checkMetrics(report, std::move(tag), expectedValue);
    }

    void checkMetrics(std::string tag,
                      int expectedValue,
                      std::string errMsg,
                      OpReportType reportType) {
        const auto report = getReport(reportType);
        checkMetrics(report, std::move(tag), expectedValue, std::move(errMsg));
    }

    void checkMetrics(const BSONObj& report,
                      std::string tag,
                      int expectedValue,
                      std::string errMsg = "Unexpected value") const {
        ASSERT_EQ(report.getIntField(tag), expectedValue)
            << fmt::format("{}: {}", errMsg, report.toString());
    };

private:
    ClockSourceMock* _clockSource;
};

// TODO Re-enable once underlying invariants are re-enabled
/*
DEATH_TEST_F(ReshardingMetricsTest, RunOnCompletionBeforeOnStart, "No operation is in progress") {
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
}

DEATH_TEST_F(ReshardingMetricsTest,
             RunOnStepUpAfterOnStartInvariants,
             "Another operation is in progress") {
    getMetrics()->onStart();
    getMetrics()->onStepUp();
}

DEATH_TEST_F(ReshardingMetricsTest,
             RunOnCompletionAfterOnStepDownInvariants,
             "No operation is in progress") {
    getMetrics()->onStart();
    getMetrics()->onStepDown();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
}
*/

TEST_F(ReshardingMetricsTest, RunOnStepDownAfterOnCompletionIsSafe) {
    getMetrics()->onStart();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
    getMetrics()->onStepDown();
}

TEST_F(ReshardingMetricsTest, OperationStatus) {
    getMetrics()->onStart();
    const auto report = getReport(OpReportType::CurrentOpReportCoordinatorRole);
    ASSERT_EQ(report.getStringField("opStatus"),
              ReshardingOperationStatus_serializer(ReshardingOperationStatusEnum::kRunning));
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
}

TEST_F(ReshardingMetricsTest, TestOperationStatus) {
    const auto kNumSuccessfulOps = 3;
    const auto kNumFailedOps = 5;
    const auto kNumCanceledOps = 7;

    for (auto i = 0; i < kNumSuccessfulOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
    }

    for (auto i = 0; i < kNumFailedOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingOperationStatusEnum::kFailure);
    }

    for (auto i = 0; i < kNumCanceledOps; i++) {
        getMetrics()->onStart();
        getMetrics()->onCompletion(ReshardingOperationStatusEnum::kCanceled);
    }

    checkMetrics("countReshardingSuccessful", kNumSuccessfulOps, OpReportType::CumulativeReport);
    checkMetrics("countReshardingFailures", kNumFailedOps, OpReportType::CumulativeReport);
    checkMetrics("countReshardingCanceled", kNumCanceledOps, OpReportType::CumulativeReport);

    const auto total = kNumSuccessfulOps + kNumFailedOps + kNumCanceledOps;
    checkMetrics("countReshardingOperations", total, OpReportType::CumulativeReport);
    getMetrics()->onStart();
    checkMetrics("countReshardingOperations", total + 1, OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, TestElapsedTime) {
    getMetrics()->onStart();
    const auto elapsedTime = 1;
    advanceTime(Seconds(elapsedTime));
    checkMetrics("totalOperationTimeElapsed", elapsedTime, OpReportType::CurrentOpReportDonorRole);
}

TEST_F(ReshardingMetricsTest, TestDonorAndRecipientMetrics) {
    getMetrics()->onStart();
    const auto elapsedTime = 1;

    advanceTime(Seconds(elapsedTime));

    // Update metrics for donor
    const auto kWritesDuringCriticalSection = 7;
    getMetrics()->setDonorState(DonorStateEnum::kDonatingOplogEntries);
    getMetrics()->startInCriticalSection();
    getMetrics()->onWriteDuringCriticalSection(kWritesDuringCriticalSection);
    advanceTime(Seconds(elapsedTime));

    // Update metrics for recipient
    const auto kDocumentsToCopy = 50;
    const auto kBytesToCopy = 740;
    const auto kCopyProgress = 50;
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy * kCopyProgress / 100,
                                    kBytesToCopy * kCopyProgress / 100);
    advanceTime(Seconds(elapsedTime));

    const auto currentDonorOpReport = getReport(OpReportType::CurrentOpReportDonorRole);
    const auto currentRecipientOpReport = getReport(OpReportType::CurrentOpReportRecipientRole);
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);

    checkMetrics(currentRecipientOpReport, "totalCopyTimeElapsed", elapsedTime);
    checkMetrics(currentRecipientOpReport, "bytesCopied", kBytesToCopy * kCopyProgress / 100);
    checkMetrics(
        currentRecipientOpReport, "documentsCopied", kDocumentsToCopy * kCopyProgress / 100);
    checkMetrics(currentDonorOpReport, "totalCriticalSectionTimeElapsed", elapsedTime * 2);
    checkMetrics(
        currentDonorOpReport, "countWritesDuringCriticalSection", kWritesDuringCriticalSection);

    // Expected remaining time = totalCopyTimeElapsed + 2 * estimated time to copy remaining
    checkMetrics(currentDonorOpReport,
                 "remainingOperationTimeEstimated",
                 elapsedTime + 2 * (100 - kCopyProgress) / kCopyProgress * elapsedTime);

    const auto cumulativeReportAfterCompletion = getReport(OpReportType::CumulativeReport);
    checkMetrics(
        cumulativeReportAfterCompletion, "bytesCopied", kBytesToCopy * kCopyProgress / 100);
    checkMetrics(
        cumulativeReportAfterCompletion, "documentsCopied", kDocumentsToCopy * kCopyProgress / 100);
    checkMetrics(cumulativeReportAfterCompletion,
                 "countWritesDuringCriticalSection",
                 kWritesDuringCriticalSection);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAreRetainedAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    advanceTime();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kFailure);
    advanceTime();

    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Cumulative metrics are not retained",
                 OpReportType::CumulativeReport);

    getMetrics()->onStart();
    checkMetrics(
        kTag, kDocumentsToCopy, "Cumulative metrics are reset", OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAreRetainedAfterCancellation) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    advanceTime();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kCanceled);
    advanceTime();

    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Cumulative metrics are not retained",
                 OpReportType::CumulativeReport);

    getMetrics()->onStart();
    checkMetrics(
        kTag, kDocumentsToCopy, "Cumulative metrics are reset", OpReportType::CumulativeReport);
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreResetAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
    advanceTime();

    getMetrics()->onStart();
    checkMetrics(
        kTag, 0, "Current metrics are not reset", OpReportType::CurrentOpReportRecipientRole);
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreNotRetainedAfterCompletion) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kFailure);
    advanceTime();

    ASSERT_FALSE(getReport(OpReportType::CurrentOpReportRecipientRole)[kTag].ok());
}

TEST_F(ReshardingMetricsTest, CurrentOpMetricsAreNotRetainedAfterStepDown) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy, kBytesToCopy);
    checkMetrics(kTag,
                 kDocumentsToCopy,
                 "Current metrics are not set",
                 OpReportType::CurrentOpReportRecipientRole);
    advanceTime();
    getMetrics()->onStepDown();
    advanceTime();

    ASSERT_FALSE(getReport(OpReportType::CurrentOpReportRecipientRole)[kTag].ok());
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTime) {
    auto constexpr kTag = "remainingOperationTimeEstimated";
    const auto elapsedTime = 1;

    getMetrics()->onStart();
    checkMetrics(kTag, -1, OpReportType::CurrentOpReportDonorRole);

    const auto kDocumentsToCopy = 2;
    const auto kBytesToCopy = 200;
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy / 2, kBytesToCopy / 2);
    advanceTime(Seconds(elapsedTime));
    // Since 50% of the data is copied, the remaining copy time equals the elapsed copy time, which
    // is equal to `elapsedTime` seconds.
    checkMetrics(kTag, elapsedTime + 2 * elapsedTime, OpReportType::CurrentOpReportDonorRole);

    const auto kOplogEntriesFetched = 4;
    const auto kOplogEntriesApplied = 2;
    getMetrics()->setRecipientState(RecipientStateEnum::kApplying);
    getMetrics()->onOplogEntriesFetched(kOplogEntriesFetched);
    getMetrics()->onOplogEntriesApplied(kOplogEntriesApplied);
    advanceTime(Seconds(elapsedTime));
    // So far, the time to apply oplog entries equals `elapsedTime` seconds.
    checkMetrics(kTag,
                 elapsedTime * (kOplogEntriesFetched / kOplogEntriesApplied - 1),
                 OpReportType::CurrentOpReportDonorRole);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForDonor) {
    const auto kDonorState = DonorStateEnum::kDonatingOplogEntries;
    getMetrics()->onStart();
    advanceTime(Seconds(2));
    getMetrics()->setDonorState(kDonorState);
    getMetrics()->startInCriticalSection();
    advanceTime(Seconds(3));

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kDonor,
        UUID::parse("12345678-1234-1234-1234-123456789abc").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        true);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingDonorService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsed: 5,"
                             "remainingOperationTimeEstimated: -1,"
                             "countWritesDuringCriticalSection: 0,"
                             "totalCriticalSectionTimeElapsed : 3,"
                             "donorState: \"{4}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             DonorState_serializer(kDonorState)));

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForRecipient) {
    const auto kRecipientState = RecipientStateEnum::kCloning;

    constexpr auto kDocumentsToCopy = 500;
    constexpr auto kDocumentsCopied = kDocumentsToCopy * 0.5;
    static_assert(kDocumentsToCopy >= kDocumentsCopied);

    constexpr auto kBytesToCopy = 8192;
    constexpr auto kBytesCopied = kBytesToCopy * 0.5;
    static_assert(kBytesToCopy >= kBytesCopied);

    constexpr auto kDelayBeforeCloning = Seconds(2);
    getMetrics()->onStart();
    advanceTime(kDelayBeforeCloning);

    constexpr auto kTimeSpentCloning = Seconds(3);
    getMetrics()->setRecipientState(RecipientStateEnum::kCreatingCollection);
    getMetrics()->setDocumentsToCopy(kDocumentsToCopy, kBytesToCopy);
    getMetrics()->setRecipientState(kRecipientState);
    advanceTime(kTimeSpentCloning);
    getMetrics()->onDocumentsCopied(kDocumentsCopied, kBytesCopied);

    const auto kTimeToCopyRemainingSeconds =
        durationCount<Seconds>(kTimeSpentCloning) * (kBytesToCopy / kBytesCopied - 1);
    const auto kRemainingOperationTimeSeconds =
        durationCount<Seconds>(kTimeSpentCloning) + 2 * kTimeToCopyRemainingSeconds;

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kRecipient,
        UUID::parse("12345678-1234-1234-1234-123456789def").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingRecipientService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsed: {4},"
                             "remainingOperationTimeEstimated: {5},"
                             "approxDocumentsToCopy: {6},"
                             "documentsCopied: {7},"
                             "approxBytesToCopy: {8},"
                             "bytesCopied: {9},"
                             "totalCopyTimeElapsed: {10},"
                             "oplogEntriesFetched: 0,"
                             "oplogEntriesApplied: 0,"
                             "totalApplyTimeElapsed: 0,"
                             "recipientState: \"{11}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             durationCount<Seconds>(kDelayBeforeCloning + kTimeSpentCloning),
                             kRemainingOperationTimeSeconds,
                             kDocumentsToCopy,
                             kDocumentsCopied,
                             kBytesToCopy,
                             kBytesCopied,
                             durationCount<Seconds>(kTimeSpentCloning),
                             RecipientState_serializer(kRecipientState)));

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, CurrentOpReportForCoordinator) {
    const auto kCoordinatorState = CoordinatorStateEnum::kInitializing;
    const auto kSomeDuration = Seconds(10);

    getMetrics()->onStart();
    getMetrics()->setCoordinatorState(kCoordinatorState);
    advanceTime(kSomeDuration);

    const ReshardingMetrics::ReporterOptions options(
        ReshardingMetrics::ReporterOptions::Role::kCoordinator,
        UUID::parse("12345678-1234-1234-1234-123456789cba").getValue(),
        NamespaceString("db", "collection"),
        BSON("id" << 1),
        false);

    const auto expected =
        fromjson(fmt::format("{{ type: \"op\","
                             "desc: \"ReshardingCoordinatorService {0}\","
                             "op: \"command\","
                             "ns: \"{1}\","
                             "originatingCommand: {{ reshardCollection: \"{1}\","
                             "key: {2},"
                             "unique: {3},"
                             "collation: {{ locale: \"simple\" }} }},"
                             "totalOperationTimeElapsed: {4},"
                             "remainingOperationTimeEstimated: -1,"
                             "coordinatorState: \"{5}\","
                             "opStatus: \"running\" }}",
                             options.id.toString(),
                             options.nss.toString(),
                             options.shardKey.toString(),
                             options.unique ? "true" : "false",
                             durationCount<Seconds>(kSomeDuration),
                             CoordinatorState_serializer(kCoordinatorState)));

    const auto report = getMetrics()->reportForCurrentOp(options);
    ASSERT_BSONOBJ_EQ(expected, report);
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTimeCloning) {
    // Copy N docs @ timePerDoc. Check the progression of the estimated time remaining.
    auto m = getMetrics();
    m->onStart();
    auto timePerDocument = Seconds(2);
    int64_t bytesPerDocument = 1024;
    int64_t documentsToCopy = 409;
    int64_t bytesToCopy = bytesPerDocument * documentsToCopy;
    m->setRecipientState(RecipientStateEnum::kCreatingCollection);
    m->setDocumentsToCopy(documentsToCopy, bytesToCopy);
    m->setRecipientState(RecipientStateEnum::kCloning);
    auto remainingTime = 2 * timePerDocument * documentsToCopy;
    double maxAbsRelErr = 0;
    for (int64_t copied = 0; copied < documentsToCopy; ++copied) {
        double output =
            getReport(OpReportType::CurrentOpReportRecipientRole)[kOpTimeRemaining].Number();
        if (copied == 0) {
            ASSERT_EQ(output, -1);
        } else {
            ASSERT_GTE(output, 0);
            auto expected = durationCount<Seconds>(remainingTime);
            // Check that error is pretty small (it should get better as the operation progresses)
            double absRelErr = std::abs((output - expected) / expected);
            ASSERT_LT(absRelErr, 0.05)
                << "output={}, expected={}, copied={}"_format(output, expected, copied);
            maxAbsRelErr = std::max(maxAbsRelErr, absRelErr);
        }
        m->onDocumentsCopied(1, bytesPerDocument);
        advanceTime(timePerDocument);
        remainingTime -= timePerDocument;
    }
    LOGV2_DEBUG(
        5422700, 3, "Max absolute relative error observed", "maxAbsRelErr"_attr = maxAbsRelErr);
}

TEST_F(ReshardingMetricsTest, EstimatedRemainingOperationTimeApplying) {
    // Perform N ops @ timePerOp. Check the progression of the estimated time remaining.
    auto m = getMetrics();
    m->onStart();
    m->setRecipientState(RecipientStateEnum::kApplying);
    // 1 extra millisecond here because otherwise an error of just 1ms will round this down to the
    // next second.
    auto timePerOp = Milliseconds(1001);
    int64_t fetched = 10000;
    m->onOplogEntriesFetched(fetched);
    auto remainingTime = timePerOp * fetched;
    double maxAbsRelErr = 0;
    for (int64_t applied = 0; applied < fetched; ++applied) {
        double output =
            getReport(OpReportType::CurrentOpReportRecipientRole)[kOpTimeRemaining].Number();
        if (applied == 0) {
            ASSERT_EQ(output, -1);
        } else {
            auto expected = durationCount<Seconds>(remainingTime);
            // Check that error is pretty small (it should get better as the operation progresses)
            double absRelErr = std::abs((output - expected) / expected);
            ASSERT_LT(absRelErr, 0.05)
                << "output={}, expected={}, applied={}"_format(output, expected, applied);
            maxAbsRelErr = std::max(maxAbsRelErr, absRelErr);
        }
        advanceTime(timePerOp);
        m->onOplogEntriesApplied(1);
        remainingTime -= timePerOp;
    }
    LOGV2_DEBUG(
        5422701, 3, "Max absolute relative error observed", "maxAbsRelErr"_attr = maxAbsRelErr);
}

TEST_F(ReshardingMetricsTest, CumulativeOpMetricsAccumulate) {
    auto constexpr kTag = "documentsCopied";
    getMetrics()->onStart();
    const auto kDocumentsToCopy1 = 2;
    const auto kBytesToCopy1 = 200;

    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy1, kBytesToCopy1);
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kFailure);

    getMetrics()->onStart();
    const auto kDocumentsToCopy2 = 3;
    const auto kBytesToCopy2 = 400;

    getMetrics()->setRecipientState(RecipientStateEnum::kCloning);
    getMetrics()->onDocumentsCopied(kDocumentsToCopy2, kBytesToCopy2);
    getMetrics()->onCompletion(ReshardingOperationStatusEnum::kFailure);

    checkMetrics(kTag,
                 kDocumentsToCopy1 + kDocumentsToCopy2,
                 "Cumulative metrics are not accumulated",
                 OpReportType::CumulativeReport);
}

}  // namespace
}  // namespace mongo

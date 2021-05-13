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

#pragma once

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

/*
 * Maintains the metrics for resharding operations.
 * All members of this class are thread-safe.
 */
class ReshardingMetrics final {
public:
    ReshardingMetrics(const ReshardingMetrics&) = delete;
    ReshardingMetrics(ReshardingMetrics&&) = delete;

    explicit ReshardingMetrics(ServiceContext* svcCtx)
        : _svcCtx(svcCtx), _cumulativeOp(svcCtx->getFastClockSource()) {}

    static ReshardingMetrics* get(ServiceContext*) noexcept;

    // Marks the beginning of a resharding operation. Not that only one resharding operation may run
    // at any time.
    void onStart() noexcept;

    // Marks the resumption of a resharding operation. Note that only one resharding operation may
    // run at any time.
    void onStepUp() noexcept;

    // So long as a resharding operation is in progress, the following may be used to update the
    // state of a donor, a recipient, and a coordinator, respectively.
    void setDonorState(DonorStateEnum) noexcept;
    void setRecipientState(RecipientStateEnum) noexcept;
    void setCoordinatorState(CoordinatorStateEnum) noexcept;

    // Set by donors.
    void setDocumentsToCopy(int64_t documents, int64_t bytes) noexcept;
    // Allows updating metrics on "documents to copy" so long as the recipient is in cloning state.
    void onDocumentsCopied(int64_t documents, int64_t bytes) noexcept;

    // Starts/ends the timer recording the time spent in the critical section.
    void startInCriticalSection();
    void endInCritcialSection();

    // Allows updating "oplog entries to apply" metrics when the recipient is in applying state.
    void onOplogEntriesFetched(int64_t entries) noexcept;
    void onOplogEntriesApplied(int64_t entries) noexcept;

    // Allows tracking writes during a critical section when the donor's state is either of
    // "donating-oplog-entries" or "blocking-writes".
    void onWriteDuringCriticalSection(int64_t writes) noexcept;

    // Tears down the currentOp variable so that the node that is stepping up may continue the
    // resharding operation from disk.
    void onStepDown() noexcept;

    // Marks the completion of the current (active) resharding operation. Aborts the process if no
    // resharding operation is in progress.
    void onCompletion(ReshardingOperationStatusEnum) noexcept;

    struct ReporterOptions {
        enum class Role { kDonor, kRecipient, kCoordinator };
        ReporterOptions(Role role, UUID id, NamespaceString nss, BSONObj shardKey, bool unique)
            : role(role),
              id(std::move(id)),
              nss(std::move(nss)),
              shardKey(std::move(shardKey)),
              unique(unique) {}

        const Role role;
        const UUID id;
        const NamespaceString nss;
        const BSONObj shardKey;
        const bool unique;
    };
    BSONObj reportForCurrentOp(const ReporterOptions& options) const noexcept;

    // Append metrics to the builder in CurrentOp format for the given `role`.
    void serializeCurrentOpMetrics(BSONObjBuilder*, ReporterOptions::Role role) const;

    // Append metrics to the builder in CumulativeOp (ServerStatus) format.
    void serializeCumulativeOpMetrics(BSONObjBuilder*) const;

    // Reports the elapsed time for the active resharding operation, or `boost::none`.
    boost::optional<Milliseconds> getOperationElapsedTime() const;

    // Reports the estimated remaining time for the active resharding operation, or `boost::none`.
    boost::optional<Milliseconds> getOperationRemainingTime() const;

private:
    ServiceContext* const _svcCtx;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReshardingMetrics::_mutex");

    // The following maintain the number of resharding operations that have started, succeeded,
    // failed with an unrecoverable error, and canceled by the user, respectively.
    int64_t _started = 0;
    int64_t _succeeded = 0;
    int64_t _failed = 0;
    int64_t _canceled = 0;

    // Metrics for resharding operation. Accesses must be serialized using `_mutex`.
    struct OperationMetrics {
        // Allows tracking elapsed time for the resharding operation and its sub operations (e.g.,
        // applying oplog entries).
        class TimeInterval {
        public:
            explicit TimeInterval(ClockSource* clockSource) : _clockSource(clockSource) {}

            void start() noexcept;

            void end() noexcept;

            Milliseconds duration() const noexcept;

        private:
            ClockSource* const _clockSource;
            boost::optional<Date_t> _start;
            boost::optional<Date_t> _end;
        };

        explicit OperationMetrics(ClockSource* clockSource)
            : runningOperation(clockSource),
              copyingDocuments(clockSource),
              applyingOplogEntries(clockSource),
              inCriticalSection(clockSource) {}

        using Role = ReporterOptions::Role;
        void appendCurrentOpMetrics(BSONObjBuilder*, Role) const;

        void appendCumulativeOpMetrics(BSONObjBuilder*) const;

        boost::optional<Milliseconds> remainingOperationTime() const;

        TimeInterval runningOperation;
        ReshardingOperationStatusEnum opStatus = ReshardingOperationStatusEnum::kInactive;

        TimeInterval copyingDocuments;
        int64_t documentsToCopy = 0;
        int64_t documentsCopied = 0;
        int64_t bytesToCopy = 0;
        int64_t bytesCopied = 0;

        TimeInterval applyingOplogEntries;
        int64_t oplogEntriesFetched = 0;
        int64_t oplogEntriesApplied = 0;

        TimeInterval inCriticalSection;
        int64_t writesDuringCriticalSection = 0;

        DonorStateEnum donorState = DonorStateEnum::kUnused;
        RecipientStateEnum recipientState = RecipientStateEnum::kUnused;
        CoordinatorStateEnum coordinatorState = CoordinatorStateEnum::kUnused;
    };
    boost::optional<OperationMetrics> _currentOp;
    OperationMetrics _cumulativeOp;
};

}  // namespace mongo

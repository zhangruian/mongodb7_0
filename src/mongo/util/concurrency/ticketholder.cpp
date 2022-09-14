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

#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace {
const auto ticketHolderDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<mongo::TicketHolder>>();
}

namespace mongo {

TicketHolder* TicketHolder::get(ServiceContext* svcCtx) {
    return ticketHolderDecoration(svcCtx).get();
}

void TicketHolder::use(ServiceContext* svcCtx, std::unique_ptr<TicketHolder> newTicketHolder) {
    ticketHolderDecoration(svcCtx) = std::move(newTicketHolder);
}

ReaderWriterTicketHolder::~ReaderWriterTicketHolder(){};

boost::optional<Ticket> ReaderWriterTicketHolder::tryAcquire(AdmissionContext* admCtx) {

    switch (admCtx->getLockMode()) {
        case MODE_IS:
        case MODE_S:
            return _reader->tryAcquire(admCtx);
        case MODE_IX:
            return _writer->tryAcquire(admCtx);
        default:
            MONGO_UNREACHABLE;
    }
}

Ticket ReaderWriterTicketHolder::waitForTicket(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               WaitMode waitMode) {
    switch (admCtx->getLockMode()) {
        case MODE_IS:
        case MODE_S:
            return _reader->waitForTicket(opCtx, admCtx, waitMode);
        case MODE_IX:
            return _writer->waitForTicket(opCtx, admCtx, waitMode);
        default:
            MONGO_UNREACHABLE;
    }
}

boost::optional<Ticket> ReaderWriterTicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                                     AdmissionContext* admCtx,
                                                                     Date_t until,
                                                                     WaitMode waitMode) {
    switch (admCtx->getLockMode()) {
        case MODE_IS:
        case MODE_S:
            return _reader->waitForTicketUntil(opCtx, admCtx, until, waitMode);
        case MODE_IX:
            return _writer->waitForTicketUntil(opCtx, admCtx, until, waitMode);
        default:
            MONGO_UNREACHABLE;
    }
}

void ReaderWriterTicketHolder::appendStats(BSONObjBuilder& b) const {
    invariant(_writer, "Writer queue is not present in the ticketholder");
    invariant(_reader, "Reader queue is not present in the ticketholder");
    {
        BSONObjBuilder bbb(b.subobjStart("write"));
        _writer->appendStats(bbb);
        bbb.done();
    }
    {
        BSONObjBuilder bbb(b.subobjStart("read"));
        _reader->appendStats(bbb);
        bbb.done();
    }
}

void ReaderWriterTicketHolder::_release(AdmissionContext* admCtx) noexcept {
    switch (admCtx->getLockMode()) {
        case MODE_IS:
        case MODE_S:
            return _reader->_release(admCtx);
        case MODE_IX:
            return _writer->_release(admCtx);
        default:
            MONGO_UNREACHABLE;
    }
}

void ReaderWriterTicketHolder::resizeReaders(int newSize) {
    return _reader->resize(newSize);
}

void ReaderWriterTicketHolder::resizeWriters(int newSize) {
    return _writer->resize(newSize);
}

void TicketHolderWithQueueingStats::resize(int newSize) noexcept {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    _resize(newSize, _outof.load());
    _outof.store(newSize);
}

void TicketHolderWithQueueingStats::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
    _appendImplStats(b);
}

void TicketHolderWithQueueingStats::_release(AdmissionContext* admCtx) noexcept {
    auto& queueStats = _getQueueStatsToUse(admCtx);
    queueStats.totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto startTime = admCtx->getStartProcessingTime();
    auto tickSource = _serviceContext->getTickSource();
    auto delta = tickSource->spanTo<Microseconds>(startTime, tickSource->getTicks());
    queueStats.totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());
    _releaseQueue(admCtx);
}

Ticket TicketHolderWithQueueingStats::waitForTicket(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    WaitMode waitMode) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max(), waitMode);
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolderWithQueueingStats::tryAcquire(AdmissionContext* admCtx) {
    invariant(admCtx);

    auto ticket = _tryAcquireImpl(admCtx);
    // Track statistics.
    if (ticket) {
        auto& queueStats = _getQueueStatsToUse(admCtx);
        if (admCtx->getAdmissions() == 0) {
            queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
        }
        admCtx->start(_serviceContext->getTickSource());
        queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);
    }
    return ticket;
}


boost::optional<Ticket> TicketHolderWithQueueingStats::waitForTicketUntil(OperationContext* opCtx,
                                                                          AdmissionContext* admCtx,
                                                                          Date_t until,
                                                                          WaitMode waitMode) {
    invariant(admCtx);

    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto& queueStats = _getQueueStatsToUse(admCtx);
    auto tickSource = _serviceContext->getTickSource();
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->spanTo<Microseconds>(oldWaitTime, currentWaitTime).count();
        queueStats.totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta);
    };
    queueStats.totalAddedQueue.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&] {
        updateQueuedTime();
        queueStats.totalRemovedQueue.fetchAndAddRelaxed(1);
    });

    ScopeGuard cancelWait([&] {
        // Update statistics.
        queueStats.totalCanceled.fetchAndAddRelaxed(1);
    });

    auto ticket = _waitForTicketUntilImpl(opCtx, admCtx, until, waitMode);

    if (ticket) {
        cancelWait.dismiss();
        if (admCtx->getAdmissions() == 0) {
            queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
        }
        admCtx->start(_serviceContext->getTickSource());
        queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);
        return ticket;
    } else {
        return boost::none;
    }
}

void SemaphoreTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    auto removed = _semaphoreStats.totalRemovedQueue.loadRelaxed();
    auto added = _semaphoreStats.totalAddedQueue.loadRelaxed();
    b.append("addedToQueue", added);
    b.append("removedFromQueue", removed);
    b.append("queueLength", std::max(static_cast<int>(added - removed), 0));
    auto finished = _semaphoreStats.totalFinishedProcessing.loadRelaxed();
    auto started = _semaphoreStats.totalStartedProcessing.loadRelaxed();
    b.append("startedProcessing", started);
    b.append("processing", std::max(static_cast<int>(started - finished), 0));
    b.append("finishedProcessing", finished);
    b.append("totalTimeProcessingMicros", _semaphoreStats.totalTimeProcessingMicros.loadRelaxed());
    b.append("canceled", _semaphoreStats.totalCanceled.loadRelaxed());
    b.append("newAdmissions", _semaphoreStats.totalNewAdmissions.loadRelaxed());
    b.append("totalTimeQueuedMicros", _semaphoreStats.totalTimeQueuedMicros.loadRelaxed());
}
#if defined(__linux__)
namespace {

/**
 * Accepts an errno code, prints its error message, and exits.
 */
void failWithErrno(int err) {
    LOGV2_FATAL(28604,
                "error in Ticketholder: {errnoWithDescription_err}",
                "errnoWithDescription_err"_attr = errorMessage(posixError(err)));
}

/*
 * Checks the return value from a Linux semaphore function call, and fails with the set errno if the
 * call was unsucessful.
 */
void check(int ret) {
    if (ret == 0)
        return;
    failWithErrno(errno);
}

/**
 * Takes a Date_t deadline and sets the appropriate values in a timespec structure.
 */
void tsFromDate(const Date_t& deadline, struct timespec& ts) {
    ts.tv_sec = deadline.toTimeT();
    ts.tv_nsec = (deadline.toMillisSinceEpoch() % 1000) * 1'000'000;
}
}  // namespace

SemaphoreTicketHolder::SemaphoreTicketHolder(int numTickets, ServiceContext* serviceContext)
    : TicketHolderWithQueueingStats(numTickets, serviceContext) {
    check(sem_init(&_sem, 0, numTickets));
}

SemaphoreTicketHolder::~SemaphoreTicketHolder() {
    check(sem_destroy(&_sem));
}

boost::optional<Ticket> SemaphoreTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    while (0 != sem_trywait(&_sem)) {
        if (errno == EAGAIN)
            return boost::none;
        if (errno != EINTR)
            failWithErrno(errno);
    }
    return Ticket{this, admCtx};
}

boost::optional<Ticket> SemaphoreTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                       AdmissionContext* admCtx,
                                                                       Date_t until,
                                                                       WaitMode waitMode) {
    const Milliseconds intervalMs(500);
    struct timespec ts;

    // To support interrupting ticket acquisition while still benefiting from semaphores, we do a
    // timed wait on an interval to periodically check for interrupts.
    // The wait period interval is the smaller of the default interval and the provided
    // deadline.
    Date_t deadline = std::min(until, Date_t::now() + intervalMs);
    tsFromDate(deadline, ts);

    while (0 != sem_timedwait(&_sem, &ts)) {
        if (errno == ETIMEDOUT) {
            // If we reached the deadline without being interrupted, we have completely timed out.
            if (deadline == until)
                return boost::none;

            deadline = std::min(until, Date_t::now() + intervalMs);
            tsFromDate(deadline, ts);
        } else if (errno != EINTR) {
            failWithErrno(errno);
        }

        // To correctly handle errors from sem_timedwait, we should check for interrupts last.
        // It is possible to unset 'errno' after a call to checkForInterrupt().
        if (waitMode == WaitMode::kInterruptible)
            opCtx->checkForInterrupt();
    }
    return Ticket{this, admCtx};
}

void SemaphoreTicketHolder::_releaseQueue(AdmissionContext* admCtx) noexcept {
    check(sem_post(&_sem));
}

int SemaphoreTicketHolder::available() const {
    int val = 0;
    check(sem_getvalue(&_sem, &val));
    return val;
}

void SemaphoreTicketHolder::_resize(int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    if (difference > 0) {
        for (int i = 0; i < difference; i++) {
            check(sem_post(&_sem));
        }
    } else if (difference < 0) {
        for (int i = 0; i < -difference; i++) {
            check(sem_wait(&_sem));
        }
    }
}

#else

SemaphoreTicketHolder::SemaphoreTicketHolder(int numTickets, ServiceContext* svcCtx)
    : TicketHolderWithQueueingStats(numTickets, svcCtx), _numTickets(numTickets) {}

SemaphoreTicketHolder::~SemaphoreTicketHolder() = default;

boost::optional<Ticket> SemaphoreTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_tryAcquire()) {
        return boost::none;
    }
    return Ticket{this, admCtx};
}

boost::optional<Ticket> SemaphoreTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                       AdmissionContext* admCtx,
                                                                       Date_t until,
                                                                       WaitMode waitMode) {
    stdx::unique_lock<Latch> lk(_mutex);

    bool taken = [&] {
        if (waitMode == WaitMode::kInterruptible) {
            return opCtx->waitForConditionOrInterruptUntil(
                _newTicket, lk, until, [this] { return _tryAcquire(); });
        } else {
            if (until == Date_t::max()) {
                _newTicket.wait(lk, [this] { return _tryAcquire(); });
                return true;
            } else {
                return _newTicket.wait_until(
                    lk, until.toSystemTimePoint(), [this] { return _tryAcquire(); });
            }
        }
    }();
    if (!taken) {
        return boost::none;
    }
    return Ticket{this, admCtx};
}

void SemaphoreTicketHolder::_releaseQueue(AdmissionContext* admCtx) noexcept {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _numTickets++;
    }
    _newTicket.notify_one();
}

int SemaphoreTicketHolder::available() const {
    return _numTickets;
}

bool SemaphoreTicketHolder::_tryAcquire() {
    if (_numTickets <= 0) {
        if (_numTickets < 0) {
            std::cerr << "DISASTER! in TicketHolder" << std::endl;
        }
        return false;
    }
    _numTickets--;
    return true;
}

void SemaphoreTicketHolder::_resize(int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    stdx::lock_guard<Latch> lk(_mutex);
    _numTickets += difference;

    if (difference > 0) {
        for (int i = 0; i < difference; i++) {
            _newTicket.notify_one();
        }
    }
    // No need to do anything in the other cases as the number of tickets being <= 0 implies they'll
    // have to wait until the current ticket holders release their tickets.
}
#endif

SchedulingTicketHolder::SchedulingTicketHolder(int numTickets,
                                               unsigned int numQueues,
                                               ServiceContext* serviceContext)
    : TicketHolderWithQueueingStats(numTickets, serviceContext), _serviceContext(serviceContext) {
    for (std::size_t i = 0; i < numQueues; i++) {
        _queues.emplace_back(this);
    }
    _queues.shrink_to_fit();
    _ticketsAvailable.store(numTickets);
    _enqueuedElements.store(0);
}

SchedulingTicketHolder::~SchedulingTicketHolder() {}

int SchedulingTicketHolder::available() const {
    return _ticketsAvailable.load();
}

int SchedulingTicketHolder::queued() const {
    return _enqueuedElements.loadRelaxed();
}

void SchedulingTicketHolder::_releaseQueue(AdmissionContext* admCtx) noexcept {
    invariant(admCtx);
    // The idea behind the release mechanism consists of a consistent view of queued elements
    // waiting for a ticket and many threads releasing tickets simultaneously. The releasers will
    // proceed to attempt to dequeue an element by seeing if there are threads not woken and waking
    // one, having increased the number of woken threads for accuracy. Once the thread gets woken it
    // will then decrease the number of woken threads (as it has been woken) and then attempt to
    // acquire a ticket. The two possible states are either one or more releasers releasing or a
    // thread waking up due to the RW mutex.
    //
    // Under this lock the queues cannot be modified in terms of someone attempting to enqueue on
    // them, only waking threads is allowed.
    ReleaserLockGuard lk(_queueMutex);  // NOLINT
    _ticketsAvailable.addAndFetch(1);
    if (std::all_of(_queues.begin(), _queues.end(), [](const Queue& queue) {
            return queue.queuedElems() == 0;
        })) {
        return;
    }
    _dequeueWaitingThread();
}

bool SchedulingTicketHolder::_tryAcquireTicket() {
    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return false;
    }
    return true;
}

boost::optional<Ticket> SchedulingTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    invariant(admCtx);

    auto hasAcquired = _tryAcquireTicket();
    if (hasAcquired) {
        return Ticket{this, admCtx};
    }
    return boost::none;
}

boost::optional<Ticket> SchedulingTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                        AdmissionContext* admCtx,
                                                                        Date_t until,
                                                                        WaitMode waitMode) {
    invariant(admCtx);

    auto& queue = _getQueueToUse(admCtx);

    bool assigned;
    {
        stdx::unique_lock lk(_queueMutex);
        _enqueuedElements.addAndFetch(1);
        ON_BLOCK_EXIT([&] { _enqueuedElements.subtractAndFetch(1); });
        assigned = queue.enqueue(opCtx, lk, until, waitMode);
    }
    if (assigned) {
        return Ticket{this, admCtx};
    } else {
        return boost::none;
    }
}

void SchedulingTicketHolder::_resize(int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    _ticketsAvailable.fetchAndAdd(difference);

    if (difference > 0) {
        // As we're adding tickets the waiting threads need to be notified that there are new
        // tickets available.
        ReleaserLockGuard lk(_queueMutex);
        for (int i = 0; i < difference; i++) {
            _dequeueWaitingThread();
        }
    }

    // No need to do anything in the other cases as the number of tickets being <= 0 implies they'll
    // have to wait until the current ticket holders release their tickets.
}

bool SchedulingTicketHolder::Queue::attemptToDequeue() {
    auto threadsToBeWoken = _threadsToBeWoken.load();
    while (threadsToBeWoken < _queuedThreads) {
        auto canDequeue = _threadsToBeWoken.compareAndSwap(&threadsToBeWoken, threadsToBeWoken + 1);
        if (canDequeue) {
            _cv.notify_one();
            return true;
        }
    }
    return false;
}

void SchedulingTicketHolder::Queue::_signalThreadWoken() {
    auto currentThreadsToBeWoken = _threadsToBeWoken.load();
    while (currentThreadsToBeWoken > 0) {
        if (_threadsToBeWoken.compareAndSwap(&currentThreadsToBeWoken,
                                             currentThreadsToBeWoken - 1)) {
            return;
        }
    }
}

bool SchedulingTicketHolder::Queue::enqueue(OperationContext* opCtx,
                                            EnqueuerLockGuard& queueLock,
                                            const Date_t& until,
                                            WaitMode waitMode) {
    _queuedThreads++;
    // Before exiting we remove ourselves from the count of queued threads, we are still holding the
    // lock here so this is safe.
    ON_BLOCK_EXIT([&] { _queuedThreads--; });

    // TODO SERVER-69179: Replace the custom version of waiting on a condition variable with what
    // comes out of SERVER-69178.
    auto clockSource = opCtx->getServiceContext()->getPreciseClockSource();
    auto baton = waitMode == WaitMode::kInterruptible ? opCtx->getBaton().get() : nullptr;

    // We need to determine the actual deadline to use.
    auto deadline = waitMode == WaitMode::kInterruptible ? std::min(until, opCtx->getDeadline())
                                                         : Date_t::max();

    do {
        // We normally would use the opCtx->waitForConditionOrInterruptUntil method for doing this
        // check. The problem is that we must call a method that signals that the thread has been
        // woken after the condition variable wait, not before which is where the predicate would
        // go.
        while (_holder->_ticketsAvailable.load() <= 0) {
            // This method must be called after getting woken in all cases, so we use a ScopeGuard
            // to handle exceptions as well as early returns.
            ON_BLOCK_EXIT([&] { _signalThreadWoken(); });
            auto waitResult = clockSource->waitForConditionUntil(_cv, queueLock, deadline, baton);
            // We check if the operation has been interrupted (timeout, killed, etc.) here.
            if (waitMode == WaitMode::kInterruptible) {
                opCtx->checkForInterrupt();
            }
            if (waitResult == stdx::cv_status::timeout)
                return false;
        }
    } while (!_holder->_tryAcquireTicket());
    return true;
}

PriorityTicketHolder::PriorityTicketHolder(int numTickets, ServiceContext* serviceContext)
    : SchedulingTicketHolder(numTickets, 2, serviceContext) {}

void PriorityTicketHolder::_dequeueWaitingThread() {
    int currentIndexQueue = static_cast<unsigned int>(QueueType::QueueTypeSize) - 1;
    while (!_queues[currentIndexQueue].attemptToDequeue()) {
        if (currentIndexQueue == 0)
            break;
        else
            currentIndexQueue--;
    }
}

void PriorityTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    {
        BSONObjBuilder bbb(b.subobjStart("lowPriority"));
        auto& queueStats =
            _queues[static_cast<unsigned int>(QueueType::LowPriorityQueue)].getStats();
        _appendPriorityStats(bbb, queueStats);
        bbb.done();
    }
    {
        BSONObjBuilder bbb(b.subobjStart("normalPriority"));
        auto& queueStats =
            _queues[static_cast<unsigned int>(QueueType::NormalPriorityQueue)].getStats();
        _appendPriorityStats(bbb, queueStats);
        bbb.done();
    }
}

void PriorityTicketHolder::_appendPriorityStats(BSONObjBuilder& b, const QueueStats& stats) const {
    auto removed = stats.totalRemovedQueue.loadRelaxed();
    auto added = stats.totalAddedQueue.loadRelaxed();

    b.append("addedToQueue", added);
    b.append("removedFromQueue", removed);
    b.append("queueLength", std::max(static_cast<int>(added - removed), 0));

    auto finished = stats.totalFinishedProcessing.loadRelaxed();
    auto started = stats.totalStartedProcessing.loadRelaxed();
    b.append("startedProcessing", started);
    b.append("processing", std::max(static_cast<int>(started - finished), 0));
    b.append("finishedProcessing", finished);
    b.append("totalTimeProcessingMicros", stats.totalTimeProcessingMicros.loadRelaxed());
    b.append("canceled", stats.totalCanceled.loadRelaxed());
    b.append("newAdmissions", stats.totalNewAdmissions.loadRelaxed());
    b.append("totalTimeQueuedMicros", stats.totalTimeQueuedMicros.loadRelaxed());
}

SchedulingTicketHolder::Queue& PriorityTicketHolder::_getQueueToUse(
    const AdmissionContext* admCtx) noexcept {
    auto priority = admCtx->getPriority();
    switch (priority) {
        case AdmissionContext::Priority::kLow:
            return _queues[static_cast<unsigned int>(QueueType::LowPriorityQueue)];
        case AdmissionContext::Priority::kNormal:
            return _queues[static_cast<unsigned int>(QueueType::NormalPriorityQueue)];
        default:
            MONGO_UNREACHABLE;
    }
}

TicketHolderWithQueueingStats::QueueStats& PriorityTicketHolder::_getQueueStatsToUse(
    const AdmissionContext* admCtx) noexcept {
    return _getQueueToUse(admCtx).getStatsToUse();
}
}  // namespace mongo

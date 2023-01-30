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

#include "mongo/transport/session_workflow.h"

#include <memory>
#include <tuple>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/executor/split_timer.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {

MONGO_FAIL_POINT_DEFINE(doNotSetMoreToCome);
MONGO_FAIL_POINT_DEFINE(beforeCompressingExhaustResponse);
MONGO_FAIL_POINT_DEFINE(sessionWorkflowDelaySendMessage);

namespace metrics_detail {

/** Applies X(id) for each SplitId */
#define EXPAND_TIME_SPLIT_IDS(X) \
    X(started)                   \
    X(receivedWork)              \
    X(processedWork)             \
    X(sentResponse)              \
    X(yielded)                   \
    X(done)                      \
    /**/

/**
 * Applies X(id, startSplit, endSplit) for each IntervalId.
 *
 * This table defines the intervals of a per-command `SessionWorkflow` loop
 * iteration as reported to a `SplitTimer`. The splits are time points, and the
 * `intervals` are durations between notable pairs of them.
 *
 *  [started]
 *  |   [receivedWork]
 *  |   |   [processedWork]
 *  |   |   |   [sentResponse]
 *  |   |   |   |   [yielded]
 *  |   |   |   |   |   [done]
 *  |<----------------->| total
 *  |   |<------------->| active
 *  |<->|   |   |   |   | receivedWork
 *  |   |<->|   |   |   | processWork
 *  |   |   |<->|   |   | sendResponse
 *  |   |   |   |<->|   | yield
 *  |   |   |   |   |<->| finalize
 */
#define EXPAND_INTERVAL_IDS(X)                   \
    X(total, started, done)                      \
    X(active, receivedWork, done)                \
    X(receiveWork, started, receivedWork)        \
    X(processWork, receivedWork, processedWork)  \
    X(sendResponse, processedWork, sentResponse) \
    X(yield, sentResponse, yielded)              \
    X(finalize, yielded, done)                   \
    /**/

#define X_ID(id, ...) id,
enum class IntervalId : size_t { EXPAND_INTERVAL_IDS(X_ID) };
enum class TimeSplitId : size_t { EXPAND_TIME_SPLIT_IDS(X_ID) };
#undef X_ID

/** Trait for the count of the elements in a packed enum. */
template <typename T>
static constexpr size_t enumExtent = 0;

#define X_COUNT(...) +1
template <>
constexpr inline size_t enumExtent<IntervalId> = EXPAND_INTERVAL_IDS(X_COUNT);
template <>
constexpr inline size_t enumExtent<TimeSplitId> = EXPAND_TIME_SPLIT_IDS(X_COUNT);
#undef X_COUNT

struct TimeSplitDef {
    TimeSplitId id;
    StringData name;
};

struct IntervalDef {
    IntervalId id;
    StringData name;
    TimeSplitId start;
    TimeSplitId end;
};

constexpr inline auto timeSplitDefs = std::array{
#define X(id) TimeSplitDef{TimeSplitId::id, #id ""_sd},
    EXPAND_TIME_SPLIT_IDS(X)
#undef X
};

constexpr inline auto intervalDefs = std::array{
#define X(id, start, end) \
    IntervalDef{IntervalId::id, #id "Millis"_sd, TimeSplitId::start, TimeSplitId::end},
    EXPAND_INTERVAL_IDS(X)
#undef X
};

#undef EXPAND_TIME_SPLIT_IDS
#undef EXPAND_INTERVAL_IDS

struct SplitTimerPolicy {
    using TimeSplitIdType = TimeSplitId;
    using IntervalIdType = IntervalId;

    static constexpr size_t numTimeSplitIds = enumExtent<TimeSplitIdType>;
    static constexpr size_t numIntervalIds = enumExtent<IntervalIdType>;

    explicit SplitTimerPolicy(ServiceEntryPoint* sep) : _sep(sep) {}

    template <typename E>
    static constexpr size_t toIdx(E e) {
        return static_cast<size_t>(e);
    }

    static constexpr StringData getName(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].name;
    }

    static constexpr TimeSplitIdType getStartSplit(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].start;
    }

    static constexpr TimeSplitIdType getEndSplit(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].end;
    }

    static constexpr StringData getName(TimeSplitIdType tsId) {
        return timeSplitDefs[toIdx(tsId)].name;
    }

    void onStart(SplitTimer<SplitTimerPolicy>* splitTimer) {
        splitTimer->notify(TimeSplitIdType::started);
    }

    void onFinish(SplitTimer<SplitTimerPolicy>* splitTimer) {
        splitTimer->notify(TimeSplitIdType::done);
        auto t = splitTimer->getSplitInterval(IntervalIdType::sendResponse);
        if (MONGO_likely(!t || *t < Milliseconds{serverGlobalParams.slowMS.load()}))
            return;
        BSONObjBuilder bob;
        splitTimer->appendIntervals(bob);

        logv2::LogSeverity severity = sessionWorkflowDelaySendMessage.shouldFail()
            ? logv2::LogSeverity::Info()
            : _sep->slowSessionWorkflowLogSeverity();

        LOGV2_DEBUG(6983000,
                    severity.toInt(),
                    "Slow network response send time",
                    "elapsed"_attr = bob.obj());
    }

    Timer makeTimer() {
        return Timer{};
    }

    ServiceEntryPoint* _sep;
};

class SessionWorkflowMetrics {
public:
    explicit SessionWorkflowMetrics(ServiceEntryPoint* sep) : _sep(sep) {}

    void start() {
        _t.emplace(SplitTimerPolicy{_sep});
    }
    void received() {
        _t->notify(TimeSplitId::receivedWork);
    }
    void processed() {
        _t->notify(TimeSplitId::processedWork);
    }
    void sent(Session& session) {
        _t->notify(TimeSplitId::sentResponse);
        IngressHandshakeMetrics::get(session).onResponseSent(
            duration_cast<Milliseconds>(*_t->getSplitInterval(IntervalId::processWork)),
            duration_cast<Milliseconds>(*_t->getSplitInterval(IntervalId::sendResponse)));
    }
    void yielded() {
        _t->notify(TimeSplitId::yielded);
    }
    void finish() {
        _t.reset();
    }

private:
    ServiceEntryPoint* _sep;
    boost::optional<SplitTimer<SplitTimerPolicy>> _t;
};
}  // namespace metrics_detail

/**
 * Given a request and its already generated response, checks for exhaust flags. If exhaust is
 * allowed, produces the subsequent request message, and modifies the response message to indicate
 * it is part of an exhaust stream. Returns the subsequent request message, which is known as a
 * 'synthetic' exhaust request. Returns an empty optional if exhaust is not allowed.
 */
boost::optional<Message> makeExhaustMessage(Message requestMsg, DbResponse& response) {
    if (!OpMsgRequest::isFlagSet(requestMsg, OpMsg::kExhaustSupported) ||
        !response.shouldRunAgainForExhaust)
        return {};

    const bool checksumPresent = OpMsg::isFlagSet(requestMsg, OpMsg::kChecksumPresent);
    Message exhaustMessage;

    if (auto nextInvocation = response.nextInvocation) {
        // The command provided a new BSONObj for the next invocation.
        OpMsgBuilder builder;
        builder.setBody(*nextInvocation);
        exhaustMessage = builder.finish();
    } else {
        // Reuse the previous invocation for the next invocation.
        OpMsg::removeChecksum(&requestMsg);
        exhaustMessage = requestMsg;
    }

    // The id of the response is used as the request id of this 'synthetic' request. Re-checksum
    // if needed.
    exhaustMessage.header().setId(response.response.header().getId());
    exhaustMessage.header().setResponseToMsgId(response.response.header().getResponseToMsgId());
    OpMsg::setFlag(&exhaustMessage, OpMsg::kExhaustSupported);
    if (checksumPresent) {
        OpMsg::appendChecksum(&exhaustMessage);
    }

    OpMsg::removeChecksum(&response.response);
    // Indicate that the response is part of an exhaust stream (unless the 'doNotSetMoreToCome'
    // failpoint is set). Re-checksum if needed.
    if (!MONGO_unlikely(doNotSetMoreToCome.shouldFail())) {
        OpMsg::setFlag(&response.response, OpMsg::kMoreToCome);
    }
    if (checksumPresent) {
        OpMsg::appendChecksum(&response.response);
    }

    return exhaustMessage;
}

/**
 * If `in` encodes a "getMore" command, make a best-effort attempt to kill its
 * cursor. Returns true if such an attempt was successful. If the killCursors request
 * fails here for any reasons, it will still be cleaned up once the cursor times
 * out.
 */
bool killExhaust(const Message& in, ServiceEntryPoint* sep, Client* client) {
    try {
        auto inRequest = OpMsgRequest::parse(in, client);
        const BSONObj& body = inRequest.body;
        const auto& [cmd, firstElement] = body.firstElement();
        if (cmd != "getMore"_sd)
            return false;
        StringData db = inRequest.getDatabase();
        sep->handleRequest(
               client->makeOperationContext().get(),
               OpMsgRequest::fromDBAndBody(
                   db,
                   KillCursorsCommandRequest(NamespaceString(db, body["collection"].String()),
                                             {CursorId{firstElement.Long()}})
                       .toBSON(BSONObj{}))
                   .serialize())
            .get();
        return true;
    } catch (const DBException& e) {
        LOGV2(22992, "Error cleaning up resources for exhaust request", "error"_attr = e);
    }
    return false;
}

}  // namespace

class SessionWorkflow::Impl {
public:
    class WorkItem;

    Impl(SessionWorkflow* workflow, ServiceContext::UniqueClient client)
        : _workflow{workflow},
          _serviceContext{client->getServiceContext()},
          _sep{_serviceContext->getServiceEntryPoint()},
          _clientStrand{ClientStrand::make(std::move(client))} {}

    ~Impl() {
        _sep->onEndSession(session());
    }

    Client* client() const {
        return _clientStrand->getClientPointer();
    }

    void start() {
        _scheduleIteration();
    }

    /*
     * Terminates the associated transport Session, regardless of tags.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminate();

    /*
     * Terminates the associated transport Session if its tags don't match the supplied tags.  If
     * the session is in a pending state, before any tags have been set, it will not be terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(Session::TagMask tags);

    const SessionHandle& session() const {
        return client()->session();
    }

    ServiceExecutor* executor() {
        return seCtx()->getServiceExecutor();
    }

    bool useDedicatedThread() {
        return seCtx()->useDedicatedThread();
    }

    std::shared_ptr<ServiceExecutor::TaskRunner> taskRunner() {
        auto exec = executor();
        // Allows switching the executor between iterations of the workflow.
        if (MONGO_unlikely(!_taskRunner.source || _taskRunner.source != exec))
            _taskRunner = {exec->makeTaskRunner(), exec};
        return _taskRunner.runner;
    }

    bool isTLS() const {
#ifdef MONGO_CONFIG_SSL
        return SSLPeerInfo::forSession(session()).isTLS();
#else
        return false;
#endif
    }

    ServiceExecutorContext* seCtx() {
        return ServiceExecutorContext::get(client());
    }

private:
    struct RunnerAndSource {
        std::shared_ptr<ServiceExecutor::TaskRunner> runner;
        ServiceExecutor* source = nullptr;
    };

    /** Alias: refers to this Impl, but holds a ref to the enclosing workflow. */
    std::shared_ptr<Impl> shared_from_this() {
        return {_workflow->shared_from_this(), this};
    }

    /**
     * Returns a callback that's just like `cb`, but runs under the `_clientStrand`.
     * The wrapper binds a `shared_from_this` so `cb` doesn't need its own copy
     * of that anchoring shared pointer.
     */
    unique_function<void(Status)> _captureContext(unique_function<void(Status)> cb) {
        return [this, a = shared_from_this(), cb = std::move(cb)](Status st) mutable {
            _clientStrand->run([&] { cb(st); });
        };
    }

    void _scheduleIteration();

    Future<void> _doOneIteration();

    /** Returns a Future for the next WorkItem. */
    Future<std::unique_ptr<WorkItem>> _getNextWork() {
        invariant(!_work);
        if (_nextWork)
            return Future{std::move(_nextWork)};  // Already have one ready.
        if (useDedicatedThread())
            return _receiveRequest();
        auto&& [p, f] = makePromiseFuture<void>();
        taskRunner()->runOnDataAvailable(
            session(), _captureContext([p = std::move(p)](Status s) mutable { p.setFrom(s); }));
        return std::move(f).then([this, anchor = shared_from_this()] { return _receiveRequest(); });
    }

    /** Receives a message from the session and creates a new WorkItem from it. */
    std::unique_ptr<WorkItem> _receiveRequest();

    /** Sends work to the ServiceEntryPoint, obtaining a future for its completion. */
    Future<DbResponse> _dispatchWork();

    /** Handles the completed response from dispatched work. */
    void _acceptResponse(DbResponse response);

    /** Writes the completed work response to the Session. */
    void _sendResponse();

    void _onLoopError(Status error);

    void _cleanupSession(const Status& status);

    /*
     * Releases all the resources associated with the exhaust request.
     * When the session is closing, the most recently synthesized exhaust
     * `WorkItem` may refer to a cursor that we won't need anymore, so we can
     * try to kill it early as an optimization.
     */
    void _cleanupExhaustResources();

    /**
     * Notify the task runner that this would be a good time to yield. It might
     * not actually yield, depending on implementation and on overall system
     * state.
     *
     * Yielding at certain points in a command's processing pipeline has been
     * considered to be beneficial to performance.
     */
    void _yieldPointReached() {
        executor()->yieldIfAppropriate();
    }

    SessionWorkflow* const _workflow;
    ServiceContext* const _serviceContext;
    ServiceEntryPoint* _sep;
    RunnerAndSource _taskRunner;

    AtomicWord<bool> _isTerminated{false};
    ClientStrandPtr _clientStrand;

    std::unique_ptr<WorkItem> _work;
    std::unique_ptr<WorkItem> _nextWork; /**< created by exhaust responses */
};

class SessionWorkflow::Impl::WorkItem {
public:
    WorkItem(Impl* swf, Message in) : _swf{swf}, _in{std::move(in)} {}

    bool isExhaust() const {
        return _isExhaust;
    }

    void initOperation() {
        auto newOpCtx = _swf->client()->makeOperationContext();
        if (_isExhaust)
            newOpCtx->markKillOnClientDisconnect();
        if (_in.operation() == dbCompressed)
            newOpCtx->setOpCompressed(true);
        _opCtx = std::move(newOpCtx);
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    const Message& in() const {
        return _in;
    }

    void decompressRequest() {
        if (_in.operation() != dbCompressed)
            return;
        MessageCompressorId cid;
        _in = uassertStatusOK(compressorMgr().decompressMessage(_in, &cid));
        _compressorId = cid;
    }

    Message compressResponse(Message msg) {
        if (!_compressorId)
            return msg;
        auto cid = *_compressorId;
        return uassertStatusOK(compressorMgr().compressMessage(msg, &cid));
    }

    bool hasCompressorId() const {
        return !!_compressorId;
    }

    Message consumeOut() {
        return std::move(*std::exchange(_out, {}));
    }

    bool hasOut() const {
        return !!_out;
    }

    void setOut(Message out) {
        _out = std::move(out);
    }

    /**
     * If the incoming message has the exhaust flag set, then we bypass the normal RPC
     * behavior. We will sink the response to the network, but we also synthesize a new
     * request, as if we sourced a new message from the network. This new request is
     * sent to the database once again to be processed. This cycle repeats as long as
     * the command indicates the exhaust stream should continue.
     */
    std::unique_ptr<WorkItem> synthesizeExhaust(DbResponse& response) {
        auto m = makeExhaustMessage(_in, response);
        if (!m)
            return nullptr;
        auto synth = std::make_unique<WorkItem>(_swf, std::move(*m));
        synth->_isExhaust = true;
        synth->_compressorId = _compressorId;
        return synth;
    }

private:
    MessageCompressorManager& compressorMgr() const {
        return MessageCompressorManager::forSession(_swf->session());
    }

    Impl* _swf;
    Message _in;
    bool _isExhaust = false;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::optional<MessageCompressorId> _compressorId;
    boost::optional<Message> _out;
};

std::unique_ptr<SessionWorkflow::Impl::WorkItem> SessionWorkflow::Impl::_receiveRequest() {
    try {
        auto msg = uassertStatusOK([&] {
            MONGO_IDLE_THREAD_BLOCK;
            return session()->sourceMessage();
        }());
        invariant(!msg.empty());
        return std::make_unique<WorkItem>(this, std::move(msg));
    } catch (const DBException& ex) {
        auto remote = session()->remote();
        const auto& status = ex.toStatus();
        if (ErrorCodes::isInterruption(status.code()) ||
            ErrorCodes::isNetworkError(status.code())) {
            LOGV2_DEBUG(
                22986,
                2,
                "Session from {remote} encountered a network error during SourceMessage: {error}",
                "Session from remote encountered a network error during SourceMessage",
                "remote"_attr = remote,
                "error"_attr = status);
        } else if (status == TransportLayer::TicketSessionClosedStatus) {
            // Our session may have been closed internally.
            LOGV2_DEBUG(22987,
                        2,
                        "Session from {remote} was closed internally during SourceMessage",
                        "remote"_attr = remote);
        } else {
            LOGV2(22988,
                  "Error receiving request from client. Ending connection from remote",
                  "error"_attr = status,
                  "remote"_attr = remote,
                  "connectionId"_attr = session()->id());
        }
        throw;
    }
}

void SessionWorkflow::Impl::_sendResponse() {
    if (!_work->hasOut())
        return;
    sessionWorkflowDelaySendMessage.execute([](auto&& data) {
        Milliseconds delay{data["millis"].safeNumberLong()};
        LOGV2(6724101, "sendMessage: failpoint-induced delay", "delay"_attr = delay);
        sleepFor(delay);
    });

    try {
        uassertStatusOK(session()->sinkMessage(_work->consumeOut()));
    } catch (const DBException& ex) {
        LOGV2(22989,
              "Error sending response to client. Ending connection from remote",
              "error"_attr = ex,
              "remote"_attr = session()->remote(),
              "connectionId"_attr = session()->id());
        throw;
    }
}

Future<DbResponse> SessionWorkflow::Impl::_dispatchWork() {
    invariant(_work);
    invariant(!_work->in().empty());

    TrafficRecorder::get(_serviceContext)
        .observe(session(), _serviceContext->getPreciseClockSource()->now(), _work->in());

    _work->decompressRequest();

    networkCounter.hitLogicalIn(_work->in().size());

    // Pass sourced Message to handler to generate response.
    _work->initOperation();

    return _sep->handleRequest(_work->opCtx(), _work->in());
}

void SessionWorkflow::Impl::_acceptResponse(DbResponse response) {
    auto&& work = *_work;
    // opCtx must be killed and delisted here so that the operation cannot show up in
    // currentOp results after the response reaches the client. Destruction of the already
    // killed opCtx is postponed for later (i.e., after completion of the future-chain) to
    // mitigate its performance impact on the critical path of execution.
    // Note that destroying futures after execution, rather that postponing the destruction
    // until completion of the future-chain, would expose the cost of destroying opCtx to
    // the critical path and result in serious performance implications.
    _serviceContext->killAndDelistOperation(work.opCtx(), ErrorCodes::OperationIsKilledAndDelisted);
    // Format our response, if we have one
    Message& toSink = response.response;
    if (toSink.empty())
        return;
    invariant(!OpMsg::isFlagSet(work.in(), OpMsg::kMoreToCome));
    invariant(!OpMsg::isFlagSet(toSink, OpMsg::kChecksumPresent));

    // Update the header for the response message.
    toSink.header().setId(nextMessageId());
    toSink.header().setResponseToMsgId(work.in().header().getId());
    if (!isTLS() && OpMsg::isFlagSet(work.in(), OpMsg::kChecksumPresent))
        OpMsg::appendChecksum(&toSink);

    // If the incoming message has the exhaust flag set, then bypass the normal RPC
    // behavior. Sink the response to the network, but also synthesize a new
    // request, as if a new message was sourced from the network. This new request is
    // sent to the database once again to be processed. This cycle repeats as long as
    // the dbresponses continue to indicate the exhaust stream should continue.
    _nextWork = work.synthesizeExhaust(response);

    networkCounter.hitLogicalOut(toSink.size());

    beforeCompressingExhaustResponse.executeIf(
        [&](auto&&) {}, [&](auto&&) { return work.hasCompressorId() && _nextWork; });

    toSink = work.compressResponse(toSink);

    TrafficRecorder::get(_serviceContext)
        .observe(session(), _serviceContext->getPreciseClockSource()->now(), toSink);

    work.setOut(std::move(toSink));
}

void SessionWorkflow::Impl::_onLoopError(Status error) {
    LOGV2_DEBUG(5763901, 2, "Terminating session due to error", "error"_attr = error);
    terminate();
    _cleanupSession(error);
}

/** Returns a Future representing the completion of one loop iteration. */
Future<void> SessionWorkflow::Impl::_doOneIteration() {
    struct Frame {
        explicit Frame(std::shared_ptr<Impl> a) : anchor{std::move(a)} {
            metrics.start();
        }
        ~Frame() {
            metrics.finish();
        }

        std::shared_ptr<Impl> anchor;
        metrics_detail::SessionWorkflowMetrics metrics{anchor->_sep};
    };

    auto fr = std::make_shared<Frame>(shared_from_this());
    return _getNextWork()
        .then([&, fr](auto work) {
            fr->metrics.received();
            invariant(!_work);
            _work = std::move(work);
            return _dispatchWork();
        })
        .then([&, fr](auto rsp) {
            _acceptResponse(std::move(rsp));
            fr->metrics.processed();
            _sendResponse();
            fr->metrics.sent(*session());
            _yieldPointReached();
            fr->metrics.yielded();
        });
}

void SessionWorkflow::Impl::_scheduleIteration() try {
    _work = nullptr;
    taskRunner()->schedule(_captureContext([&](Status status) {
        if (MONGO_unlikely(!status.isOK())) {
            _cleanupSession(status);
            return;
        }
        if (useDedicatedThread()) {
            try {
                _doOneIteration().get();
                _scheduleIteration();
            } catch (const DBException& ex) {
                _onLoopError(ex.toStatus());
            }
        } else {
            _doOneIteration().getAsync([this, anchor = shared_from_this()](Status st) {
                if (!st.isOK()) {
                    _onLoopError(st);
                    return;
                }
                _scheduleIteration();
            });
        }
    }));
} catch (const DBException& ex) {
    auto error = ex.toStatus();
    LOGV2_WARNING_OPTIONS(22993,
                          {logv2::LogComponent::kExecutor},
                          "Unable to schedule a new loop for the session workflow",
                          "error"_attr = error);
    _onLoopError(error);
}

void SessionWorkflow::Impl::terminate() {
    if (_isTerminated.swap(true))
        return;

    session()->end();
}

void SessionWorkflow::Impl::terminateIfTagsDontMatch(Session::TagMask tags) {
    if (_isTerminated.load())
        return;

    auto sessionTags = session()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have been
    // set, then skip the termination check.
    if ((sessionTags & tags) || (sessionTags & Session::kPending)) {
        LOGV2(
            22991, "Skip closing connection for connection", "connectionId"_attr = session()->id());
        return;
    }

    terminate();
}

void SessionWorkflow::Impl::_cleanupExhaustResources() {
    auto clean = [&](auto& w) {
        return w && w->isExhaust() && killExhaust(w->in(), _sep, client());
    };
    clean(_nextWork) || clean(_work);
}

void SessionWorkflow::Impl::_cleanupSession(const Status& status) {
    LOGV2_DEBUG(5127900, 2, "Ending session", "error"_attr = status);
    _cleanupExhaustResources();
    _taskRunner = {};
    _sep->onClientDisconnect(client());
}

SessionWorkflow::SessionWorkflow(PassKeyTag, ServiceContext::UniqueClient client)
    : _impl{std::make_unique<Impl>(this, std::move(client))} {}

SessionWorkflow::~SessionWorkflow() = default;

Client* SessionWorkflow::client() const {
    return _impl->client();
}

void SessionWorkflow::start() {
    _impl->start();
}

void SessionWorkflow::terminate() {
    _impl->terminate();
}

void SessionWorkflow::terminateIfTagsDontMatch(Session::TagMask tags) {
    _impl->terminateIfTagsDontMatch(tags);
}

}  // namespace mongo::transport

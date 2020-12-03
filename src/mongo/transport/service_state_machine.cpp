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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_state_machine.h"

#include <memory>

#include "mongo/config.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace transport {
namespace {
MONGO_FAIL_POINT_DEFINE(doNotSetMoreToCome);
/**
 * Creates and returns a legacy exhaust message, if exhaust is allowed. The returned message is to
 * be used as the subsequent 'synthetic' exhaust request. Returns an empty message if exhaust is not
 * allowed. Any messages that do not have an opcode of OP_MSG are considered legacy.
 */
Message makeLegacyExhaustMessage(Message* m, const DbResponse& dbresponse) {
    // OP_QUERY responses are always of type OP_REPLY.
    invariant(dbresponse.response.operation() == opReply);

    if (!dbresponse.shouldRunAgainForExhaust) {
        return Message();
    }

    // Legacy find operations via the OP_QUERY/OP_GET_MORE network protocol never provide the next
    // invocation for exhaust.
    invariant(!dbresponse.nextInvocation);

    DbMessage dbmsg(*m);
    invariant(dbmsg.messageShouldHaveNs());
    const char* ns = dbmsg.getns();

    MsgData::View header = dbresponse.response.header();
    QueryResult::View qr = header.view2ptr();
    long long cursorid = qr.getCursorId();

    if (cursorid == 0) {
        return Message();
    }

    // Generate a message that will act as the subsequent 'synthetic' exhaust request.
    BufBuilder b(512);
    b.appendNum(static_cast<int>(0));          // size set later in setLen()
    b.appendNum(header.getId());               // message id
    b.appendNum(header.getResponseToMsgId());  // in response to
    b.appendNum(static_cast<int>(dbGetMore));  // opCode is OP_GET_MORE
    b.appendNum(static_cast<int>(0));          // Must be ZERO (reserved)
    b.appendStr(StringData(ns));               // Namespace
    b.appendNum(static_cast<int>(0));          // ntoreturn
    b.appendNum(cursorid);                     // cursor id from the OP_REPLY

    MsgData::View(b.buf()).setLen(b.len());

    return Message(b.release());
}

/**
 * Given a request and its already generated response, checks for exhaust flags. If exhaust is
 * allowed, produces the subsequent request message, and modifies the response message to indicate
 * it is part of an exhaust stream. Returns the subsequent request message, which is known as a
 * 'synthetic' exhaust request. Returns an empty message if exhaust is not allowed.
 */
Message makeExhaustMessage(Message requestMsg, DbResponse* dbresponse) {
    if (requestMsg.operation() == dbQuery) {
        return makeLegacyExhaustMessage(&requestMsg, *dbresponse);
    }

    if (!OpMsgRequest::isFlagSet(requestMsg, OpMsg::kExhaustSupported)) {
        return Message();
    }

    if (!dbresponse->shouldRunAgainForExhaust) {
        return Message();
    }

    const bool checksumPresent = OpMsg::isFlagSet(requestMsg, OpMsg::kChecksumPresent);
    Message exhaustMessage;

    if (auto nextInvocation = dbresponse->nextInvocation) {
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
    exhaustMessage.header().setId(dbresponse->response.header().getId());
    exhaustMessage.header().setResponseToMsgId(dbresponse->response.header().getResponseToMsgId());
    OpMsg::setFlag(&exhaustMessage, OpMsg::kExhaustSupported);
    if (checksumPresent) {
        OpMsg::appendChecksum(&exhaustMessage);
    }

    OpMsg::removeChecksum(&dbresponse->response);
    // Indicate that the response is part of an exhaust stream (unless the 'doNotSetMoreToCome'
    // failpoint is set). Re-checksum if needed.
    if (!MONGO_unlikely(doNotSetMoreToCome.shouldFail())) {
        OpMsg::setFlag(&dbresponse->response, OpMsg::kMoreToCome);
    }
    if (checksumPresent) {
        OpMsg::appendChecksum(&dbresponse->response);
    }

    return exhaustMessage;
}
}  // namespace

/*
 * This class wraps up the logic for swapping/unswapping the Client when transitioning
 * between states.
 *
 * In debug builds this also ensures that only one thread is working on the SSM at once.
 */
class ServiceStateMachine::ThreadGuard {
    ThreadGuard(ThreadGuard&) = delete;
    ThreadGuard& operator=(ThreadGuard&) = delete;

public:
    explicit ThreadGuard(ServiceStateMachine* ssm) : _ssm{ssm} {
        invariant(_ssm);

        if (_ssm->_clientPtr == Client::getCurrent()) {
            // We're not the first on this thread, nothing more to do.
            return;
        }

        auto& client = _ssm->_client;
        invariant(client);

        // Set up the thread name
        auto oldThreadName = getThreadName();
        const auto& threadName = client->desc();
        if (oldThreadName != threadName) {
            _oldThreadName = oldThreadName.toString();
            setThreadName(threadName);
        }

        // Swap the current Client so calls to cc() work as expected
        Client::setCurrent(std::move(client));
        _haveTakenOwnership = true;
    }

    // Constructing from a moved ThreadGuard invalidates the other thread guard.
    ThreadGuard(ThreadGuard&& other)
        : _ssm{std::exchange(other._ssm, nullptr)},
          _haveTakenOwnership{std::exchange(_haveTakenOwnership, false)} {}

    ThreadGuard& operator=(ThreadGuard&& other) {
        _ssm = std::exchange(other._ssm, nullptr);
        _haveTakenOwnership = std::exchange(other._haveTakenOwnership, false);
        return *this;
    };

    ThreadGuard() = delete;

    ~ThreadGuard() {
        release();
    }

    explicit operator bool() const {
        return _ssm;
    }

    void release() {
        if (!_ssm) {
            // We've been released or moved from.
            return;
        }

        // If we have a ServiceStateMachine pointer, then it should control the current Client.
        invariant(_ssm->_clientPtr == Client::getCurrent());

        if (auto haveTakenOwnership = std::exchange(_haveTakenOwnership, false);
            !haveTakenOwnership) {
            // Reset our pointer so that we cannot release again.
            _ssm = nullptr;

            // We are not the original owner, nothing more to do.
            return;
        }

        // Reclaim the client.
        _ssm->_client = Client::releaseCurrent();

        // Reset our pointer so that we cannot release again.
        _ssm = nullptr;

        if (!_oldThreadName.empty()) {
            // Reset the old thread name.
            setThreadName(_oldThreadName);
        }
    }

private:
    ServiceStateMachine* _ssm = nullptr;

    bool _haveTakenOwnership = false;
    std::string _oldThreadName;
};

ServiceStateMachine::ServiceStateMachine(ServiceContext::UniqueClient client)
    : _state{State::Created},
      _serviceContext{client->getServiceContext()},
      _sep{_serviceContext->getServiceEntryPoint()},
      _client{std::move(client)},
      _clientPtr{_client.get()} {}

const transport::SessionHandle& ServiceStateMachine::_session() {
    return _clientPtr->session();
}

ServiceExecutor* ServiceStateMachine::_executor() {
    return ServiceExecutorContext::get(_clientPtr)->getServiceExecutor();
}

Future<void> ServiceStateMachine::_sourceMessage() {
    auto guard = ThreadGuard(this);

    invariant(_inMessage.empty());
    invariant(_state.load() == State::Source);
    _state.store(State::SourceWait);

    auto sourceMsgImpl = [&] {
        const auto& transportMode = _executor()->transportMode();
        if (transportMode == transport::Mode::kSynchronous) {
            MONGO_IDLE_THREAD_BLOCK;
            return Future<Message>::makeReady(_session()->sourceMessage());
        } else {
            invariant(transportMode == transport::Mode::kAsynchronous);
            return _session()->asyncSourceMessage();
        }
    };

    return sourceMsgImpl().onCompletion([this](StatusWith<Message> msg) -> Future<void> {
        if (msg.isOK()) {
            _inMessage = std::move(msg.getValue());
            invariant(!_inMessage.empty());
        }
        _sourceCallback(msg.getStatus());
        return Status::OK();
    });
}

Future<void> ServiceStateMachine::_sinkMessage() {
    auto guard = ThreadGuard(this);

    // Sink our response to the client
    invariant(_state.load() == State::Process);
    _state.store(State::SinkWait);
    auto toSink = std::exchange(_outMessage, {});

    auto sinkMsgImpl = [&] {
        const auto& transportMode = _executor()->transportMode();
        if (transportMode == transport::Mode::kSynchronous) {
            // We don't consider ourselves idle while sending the reply since we are still doing
            // work on behalf of the client. Contrast that with sourceMessage() where we are waiting
            // for the client to send us more work to do.
            return Future<void>::makeReady(_session()->sinkMessage(std::move(toSink)));
        } else {
            invariant(transportMode == transport::Mode::kAsynchronous);
            return _session()->asyncSinkMessage(std::move(toSink));
        }
    };

    return sinkMsgImpl().onCompletion([this](Status status) {
        _sinkCallback(std::move(status));
        return Status::OK();
    });
}

void ServiceStateMachine::_sourceCallback(Status status) {
    auto guard = ThreadGuard(this);

    invariant(state() == State::SourceWait);

    auto remote = _session()->remote();

    if (status.isOK()) {
        _state.store(State::Process);

        // If the sourceMessage succeeded then we can move to on to process the message. We
        // simply return from here and the future chain in _runOnce() will continue to the
        // next state normally.

        // If any other issues arise, close the session.
    } else if (ErrorCodes::isInterruption(status.code()) ||
               ErrorCodes::isNetworkError(status.code())) {
        LOGV2_DEBUG(
            22986,
            2,
            "Session from {remote} encountered a network error during SourceMessage: {error}",
            "Session from remote encountered a network error during SourceMessage",
            "remote"_attr = remote,
            "error"_attr = status);
        _state.store(State::EndSession);
    } else if (status == TransportLayer::TicketSessionClosedStatus) {
        // Our session may have been closed internally.
        LOGV2_DEBUG(22987,
                    2,
                    "Session from {remote} was closed internally during SourceMessage",
                    "remote"_attr = remote);
        _state.store(State::EndSession);
    } else {
        LOGV2(22988,
              "Error receiving request from client. Ending connection from remote",
              "error"_attr = status,
              "remote"_attr = remote,
              "connectionId"_attr = _session()->id());
        _state.store(State::EndSession);
    }
    uassertStatusOK(status);
}

void ServiceStateMachine::_sinkCallback(Status status) {
    auto guard = ThreadGuard(this);

    invariant(state() == State::SinkWait);

    // If there was an error sinking the message to the client, then we should print an error and
    // end the session.
    //
    // Otherwise, update the current state depending on whether we're in exhaust or not and return
    // from this function to let _runOnce continue the future chaining of state transitions.
    if (!status.isOK()) {
        LOGV2(22989,
              "Error sending response to client. Ending connection from remote",
              "error"_attr = status,
              "remote"_attr = _session()->remote(),
              "connectionId"_attr = _session()->id());
        _state.store(State::EndSession);
        uassertStatusOK(status);
    } else if (_inExhaust) {
        _state.store(State::Process);
    } else {
        _state.store(State::Source);
    }
}

Future<void> ServiceStateMachine::_processMessage() {
    auto guard = ThreadGuard(this);

    invariant(!_inMessage.empty());

    TrafficRecorder::get(_serviceContext)
        .observe(_session(), _serviceContext->getPreciseClockSource()->now(), _inMessage);

    auto& compressorMgr = MessageCompressorManager::forSession(_session());

    _compressorId = boost::none;
    if (_inMessage.operation() == dbCompressed) {
        MessageCompressorId compressorId;
        auto swm = compressorMgr.decompressMessage(_inMessage, &compressorId);
        uassertStatusOK(swm.getStatus());
        _inMessage = swm.getValue();
        _compressorId = compressorId;
    }

    networkCounter.hitLogicalIn(_inMessage.size());

    // Pass sourced Message to handler to generate response.
    auto opCtx = Client::getCurrent()->makeOperationContext();
    if (_inExhaust) {
        opCtx->markKillOnClientDisconnect();
    }

    // The handleRequest is implemented in a subclass for mongod/mongos and actually all the
    // database work for this request.
    return _sep->handleRequest(opCtx.get(), _inMessage)
        .then([this, &compressorMgr = compressorMgr, opCtx = std::move(opCtx)](
                  DbResponse dbresponse) mutable -> void {
            auto guard = ThreadGuard(this);

            // opCtx must be killed and delisted here so that the operation cannot show up in
            // currentOp results after the response reaches the client. The destruction is postponed
            // for later to mitigate its performance impact on the critical path of execution.
            _serviceContext->killAndDelistOperation(opCtx.get(),
                                                    ErrorCodes::OperationIsKilledAndDelisted);
            invariant(!_killedOpCtx);
            _killedOpCtx = std::move(opCtx);

            // Format our response, if we have one
            Message& toSink = dbresponse.response;
            if (!toSink.empty()) {
                invariant(!OpMsg::isFlagSet(_inMessage, OpMsg::kMoreToCome));
                invariant(!OpMsg::isFlagSet(toSink, OpMsg::kChecksumPresent));

                // Update the header for the response message.
                toSink.header().setId(nextMessageId());
                toSink.header().setResponseToMsgId(_inMessage.header().getId());
                if (OpMsg::isFlagSet(_inMessage, OpMsg::kChecksumPresent)) {
#ifdef MONGO_CONFIG_SSL
                    if (!SSLPeerInfo::forSession(_session()).isTLS) {
                        OpMsg::appendChecksum(&toSink);
                    }
#else
                    OpMsg::appendChecksum(&toSink);
#endif
                }

                // If the incoming message has the exhaust flag set, then we bypass the normal RPC
                // behavior. We will sink the response to the network, but we also synthesize a new
                // request, as if we sourced a new message from the network. This new request is
                // sent to the database once again to be processed. This cycle repeats as long as
                // the command indicates the exhaust stream should continue.
                _inMessage = makeExhaustMessage(_inMessage, &dbresponse);
                _inExhaust = !_inMessage.empty();

                networkCounter.hitLogicalOut(toSink.size());

                if (_compressorId) {
                    auto swm = compressorMgr.compressMessage(toSink, &_compressorId.value());
                    uassertStatusOK(swm.getStatus());
                    toSink = swm.getValue();
                }

                TrafficRecorder::get(_serviceContext)
                    .observe(_session(), _serviceContext->getPreciseClockSource()->now(), toSink);

                _outMessage = std::move(toSink);
            } else {
                _state.store(State::Source);
                _inMessage.reset();
                _outMessage.reset();
                _inExhaust = false;
            }
        });
}

void ServiceStateMachine::start(ServiceExecutorContext seCtx) {
    {
        stdx::lock_guard lk(*_clientPtr);

        ServiceExecutorContext::set(_clientPtr, std::move(seCtx));
    }

    _executor()->schedule(
        GuaranteedExecutor::enforceRunOnce([this, anchor = shared_from_this()](Status status) {
            // If this is the first run of the SSM, then update its state to Source
            if (state() == State::Created) {
                _state.store(State::Source);
            }

            _runOnce();
        }));
}

void ServiceStateMachine::_runOnce() {
    makeReadyFutureWith([&]() -> Future<void> {
        if (_inExhaust) {
            return Status::OK();
        } else {
            return _sourceMessage();
        }
    })
        .then([this]() { return _processMessage(); })
        .then([this]() -> Future<void> {
            if (_outMessage.empty()) {
                return Status::OK();
            }

            return _sinkMessage();
        })
        .getAsync([this, anchor = shared_from_this()](Status status) {
            // Destroy the opCtx (already killed) here, to potentially use the delay between
            // clients' requests to hide the destruction cost.
            if (MONGO_likely(_killedOpCtx)) {
                _killedOpCtx.reset();
            }
            if (!status.isOK()) {
                _state.store(State::EndSession);
                // The service executor failed to schedule the task. This could for example be
                // that we failed to start a worker thread. Terminate this connection to leave
                // the system in a valid state.
                LOGV2_WARNING_OPTIONS(4910400,
                                      {logv2::LogComponent::kExecutor},
                                      "Terminating session due to error: {error}",
                                      "Terminating session due to error",
                                      "error"_attr = status);
                terminate();

                _executor()->schedule(GuaranteedExecutor::enforceRunOnce(
                    [this, anchor = shared_from_this()](Status status) { _cleanupSession(); }));
                return;
            }

            _executor()->schedule(GuaranteedExecutor::enforceRunOnce(
                [this, anchor = shared_from_this()](Status status) { _runOnce(); }));
        });
}

void ServiceStateMachine::terminate() {
    if (state() == State::Ended)
        return;

    _session()->end();
}

void ServiceStateMachine::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    if (state() == State::Ended)
        return;

    auto sessionTags = _session()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have
    // been set, then skip the termination check.
    if ((sessionTags & tags) || (sessionTags & transport::Session::kPending)) {
        LOGV2(22991,
              "Skip closing connection for connection",
              "connectionId"_attr = _session()->id());
        return;
    }

    terminate();
}

void ServiceStateMachine::setCleanupHook(std::function<void()> hook) {
    invariant(state() == State::Created);
    _cleanupHook = std::move(hook);
}

ServiceStateMachine::State ServiceStateMachine::state() {
    return _state.load();
}

void ServiceStateMachine::_terminateAndLogIfError(Status status) {
    if (!status.isOK()) {
        LOGV2_WARNING_OPTIONS(22993,
                              {logv2::LogComponent::kExecutor},
                              "Terminating session due to error: {error}",
                              "Terminating session due to error",
                              "error"_attr = status);
        terminate();
    }
}

void ServiceStateMachine::_cleanupExhaustResources() noexcept try {
    if (!_inExhaust) {
        return;
    }
    auto request = OpMsgRequest::parse(_inMessage);
    // Clean up cursor for exhaust getMore request.
    if (request.getCommandName() == "getMore"_sd) {
        auto cursorId = request.body["getMore"].Long();
        auto opCtx = Client::getCurrent()->makeOperationContext();
        // Fire and forget. This is a best effort attempt to immediately clean up the exhaust
        // cursor. If the killCursors request fails here for any reasons, it will still be
        // cleaned up once the cursor times out.
        _sep->handleRequest(opCtx.get(), makeKillCursorsMessage(cursorId)).get();
    }
} catch (const DBException& e) {
    LOGV2(22992,
          "Error cleaning up resources for exhaust requests: {error}",
          "Error cleaning up resources for exhaust requests",
          "error"_attr = e.toStatus());
}

void ServiceStateMachine::_cleanupSession() {
    auto guard = ThreadGuard(this);

    // Ensure the delayed destruction of opCtx always happens before doing the cleanup.
    if (MONGO_likely(_killedOpCtx)) {
        _killedOpCtx.reset();
    }
    invariant(!_killedOpCtx);

    _cleanupExhaustResources();

    {
        stdx::lock_guard lk(*_clientPtr);
        transport::ServiceExecutorContext::reset(_clientPtr);
    }

    if (auto cleanupHook = std::exchange(_cleanupHook, {})) {
        cleanupHook();
    }

    _state.store(State::Ended);

    _inMessage.reset();

    _outMessage.reset();
}

}  // namespace transport
}  // namespace mongo

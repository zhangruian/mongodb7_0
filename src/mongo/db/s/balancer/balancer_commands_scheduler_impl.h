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

#pragma once

#include <list>
#include <unordered_map>

#include "mongo/db/s/balancer/balancer_command_document_gen.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

/*
 * Data structure generated from RequestData to support the creation of SchedulerResponse objects.
 */
struct ResponseHandle {
    ResponseHandle(UUID requestId,
                   const std::shared_ptr<Notification<executor::RemoteCommandResponse>>& handle)
        : requestId(requestId), handle(handle) {}

    ResponseHandle(UUID requestId, Status outcome)
        : requestId(requestId),
          handle(std::make_shared<Notification<executor::RemoteCommandResponse>>()) {
        handle->set(outcome);
    }

    ResponseHandle(ResponseHandle&& rhs)
        : requestId(rhs.requestId), handle(std::move(rhs.handle)) {}

    const UUID requestId;
    const std::shared_ptr<Notification<executor::RemoteCommandResponse>> handle;
};

/*
 * Base class exposing common methods to access and expose the outcome of requests received by
 * BalancerCommandSchedulerImpl.
 */
class SchedulerResponse {
public:
    SchedulerResponse(ResponseHandle&& responseHandle)
        : _requestId(responseHandle.requestId), _deferredValue(std::move(responseHandle.handle)) {}

    SchedulerResponse(const SchedulerResponse& other) = delete;

    ~SchedulerResponse() = default;

    UUID getRequestId() const {
        return _requestId;
    }

    bool hasFinalised() const {
        return !!(*_deferredValue);
    }

    Status getOutcome() {
        auto response = getRemoteResponse();
        if (!response.status.isOK()) {
            return response.status;
        }
        return getStatusFromCommandResult(response.data);
    }

    executor::RemoteCommandResponse getRemoteResponse() {
        return _deferredValue->get();
    }

    void setRemoteResponse(const executor::TaskExecutor::ResponseStatus& response) {
        _deferredValue->set(response);
    }

private:
    UUID _requestId;
    std::shared_ptr<Notification<executor::RemoteCommandResponse>> _deferredValue;
};


/*
 * Set of command-specific classes exposing and deserialising the outcome of
 * of requests received by BalancerCommandSchedulerImpl.
 */
class MoveChunkResponseImpl : public SchedulerResponse, public MoveChunkResponse {
public:
    MoveChunkResponseImpl(ResponseHandle&& responseHandle)
        : SchedulerResponse(std::move(responseHandle)) {}

    UUID getRequestId() const override {
        return SchedulerResponse::getRequestId();
    }

    bool hasFinalised() const override {
        return SchedulerResponse::hasFinalised();
    }

    Status getOutcome() override {
        return SchedulerResponse::getOutcome();
    }
};

class MergeChunksResponseImpl : public SchedulerResponse, public MergeChunksResponse {
public:
    MergeChunksResponseImpl(ResponseHandle&& responseHandle)
        : SchedulerResponse(std::move(responseHandle)) {}

    UUID getRequestId() const override {
        return SchedulerResponse::getRequestId();
    }

    bool hasFinalised() const override {
        return SchedulerResponse::hasFinalised();
    }

    Status getOutcome() override {
        return SchedulerResponse::getOutcome();
    }
};

class SplitVectorResponseImpl : public SchedulerResponse, public SplitVectorResponse {
public:
    SplitVectorResponseImpl(ResponseHandle&& responseHandle)
        : SchedulerResponse(std::move(responseHandle)) {}

    UUID getRequestId() const override {
        return SchedulerResponse::getRequestId();
    }

    bool hasFinalised() const override {
        return SchedulerResponse::hasFinalised();
    }

    Status getOutcome() override {
        return SchedulerResponse::getOutcome();
    }

    StatusWith<std::vector<BSONObj>> getSplitKeys() override {
        auto response = getRemoteResponse();
        if (!response.status.isOK()) {
            return response.status;
        }
        auto commandStatus = getStatusFromCommandResult(response.data);
        if (!commandStatus.isOK()) {
            return commandStatus;
        }

        std::vector<BSONObj> splitKeys;
        BSONObjIterator it(response.data.getObjectField("splitKeys"));
        while (it.more()) {
            splitKeys.emplace_back(it.next().Obj().getOwned());
        }
        return splitKeys;
    }
};

class SplitChunkResponseImpl : public SchedulerResponse, public SplitChunkResponse {
public:
    SplitChunkResponseImpl(ResponseHandle&& responseHandle)
        : SchedulerResponse(std::move(responseHandle)) {}

    UUID getRequestId() const override {
        return SchedulerResponse::getRequestId();
    }

    bool hasFinalised() const override {
        return SchedulerResponse::hasFinalised();
    }

    Status getOutcome() override {
        return SchedulerResponse::getOutcome();
    }
};

class ChunkDataSizeResponseImpl : public SchedulerResponse, public ChunkDataSizeResponse {
public:
    ChunkDataSizeResponseImpl(ResponseHandle&& responseHandle)
        : SchedulerResponse(std::move(responseHandle)) {}

    UUID getRequestId() const override {
        return SchedulerResponse::getRequestId();
    }

    bool hasFinalised() const override {
        return SchedulerResponse::hasFinalised();
    }

    Status getOutcome() override {
        return SchedulerResponse::getOutcome();
    }

    StatusWith<long long> getSize() override {
        auto response = getRemoteResponse();
        if (!response.status.isOK()) {
            return response.status;
        }
        auto commandStatus = getStatusFromCommandResult(response.data);
        if (!commandStatus.isOK()) {
            return commandStatus;
        }
        return response.data["size"].number();
    }

    StatusWith<long long> getNumObjects() override {
        auto response = getRemoteResponse();
        if (!response.status.isOK()) {
            return response.status;
        }
        auto commandStatus = getStatusFromCommandResult(response.data);
        if (!commandStatus.isOK()) {
            return commandStatus;
        }
        return response.data["numObjects"].number();
    }
};


/**
 * Base class describing the common traits of a shard command associated to a Request
 * received by BalancerCommandSchedulerImpl.
 */
class CommandInfo {
public:
    CommandInfo(const ShardId& targetShardId, const NamespaceString& nss)
        : _targetShardId(targetShardId), _nss(nss) {}

    virtual ~CommandInfo() {}

    virtual BSONObj serialise() const = 0;

    virtual bool requiresRecoveryOnCrash() const {
        return false;
    }

    virtual bool requiresDistributedLock() const {
        return false;
    }

    const ShardId& getTarget() const {
        return _targetShardId;
    }

    const NamespaceString& getNameSpace() const {
        return _nss;
    }

private:
    ShardId _targetShardId;
    NamespaceString _nss;
};

/**
 * Set of command-specific subclasses of CommandInfo.
 */
class MoveChunkCommandInfo : public CommandInfo {
public:
    MoveChunkCommandInfo(const NamespaceString& nss,
                         const ShardId& origin,
                         const ShardId& recipient,
                         const BSONObj& lowerBoundKey,
                         const BSONObj& upperBoundKey,
                         int64_t maxChunkSizeBytes,
                         const MigrationSecondaryThrottleOptions& secondaryThrottle,
                         bool waitForDelete,
                         MoveChunkRequest::ForceJumbo forceJumbo,
                         const ChunkVersion& version)
        : CommandInfo(origin, nss),
          _chunkBoundaries(lowerBoundKey, upperBoundKey),
          _recipient(recipient),
          _version(version),
          _maxChunkSizeBytes(maxChunkSizeBytes),
          _secondaryThrottle(secondaryThrottle),
          _waitForDelete(waitForDelete),
          _forceJumbo(forceJumbo) {}

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        MoveChunkRequest::appendAsCommand(&commandBuilder,
                                          getNameSpace(),
                                          _version,
                                          getTarget(),
                                          _recipient,
                                          _chunkBoundaries,
                                          _maxChunkSizeBytes,
                                          _secondaryThrottle,
                                          _waitForDelete,
                                          _forceJumbo);
        return commandBuilder.obj();
    }

    bool requiresRecoveryOnCrash() const override {
        return true;
    }

    bool requiresDistributedLock() const override {
        return true;
    }

private:
    ChunkRange _chunkBoundaries;
    ShardId _recipient;
    ChunkVersion _version;
    int64_t _maxChunkSizeBytes;
    MigrationSecondaryThrottleOptions _secondaryThrottle;
    bool _waitForDelete;
    MoveChunkRequest::ForceJumbo _forceJumbo;
};

class MergeChunksCommandInfo : public CommandInfo {
public:
    MergeChunksCommandInfo(const NamespaceString& nss,
                           const ShardId& shardId,
                           const BSONObj& lowerBoundKey,
                           const BSONObj& upperBoundKey,
                           const ChunkVersion& version)
        : CommandInfo(shardId, nss),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _version(version) {}

    BSONObj serialise() const override {
        BSONArrayBuilder boundsArrayBuilder;
        boundsArrayBuilder.append(_lowerBoundKey).append(_upperBoundKey);

        BSONObjBuilder commandBuilder;
        commandBuilder.append(kCommandName, getNameSpace().toString())
            .appendArray(kBounds, boundsArrayBuilder.arr())
            .append(kShardName, getTarget().toString())
            .append(kEpoch, _version.epoch());

        _version.appendToCommand(&commandBuilder);

        return commandBuilder.obj();
    }

private:
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    ChunkVersion _version;

    static const std::string kCommandName;
    static const std::string kBounds;
    static const std::string kShardName;
    static const std::string kEpoch;
    static const std::string kConfig;
};

class SplitVectorCommandInfo : public CommandInfo {
public:
    SplitVectorCommandInfo(const NamespaceString& nss,
                           const ShardId& shardId,
                           const BSONObj& shardKeyPattern,
                           const BSONObj& lowerBoundKey,
                           const BSONObj& upperBoundKey,
                           boost::optional<long long> maxSplitPoints,
                           boost::optional<long long> maxChunkObjects,
                           boost::optional<long long> maxChunkSizeBytes,
                           bool force)
        : CommandInfo(shardId, nss),
          _shardKeyPattern(shardKeyPattern),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _maxSplitPoints(maxSplitPoints),
          _maxChunkObjects(maxChunkObjects),
          _maxChunkSizeBytes(maxChunkSizeBytes),
          _force(force) {}

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        commandBuilder.append(kCommandName, getNameSpace().toString())
            .append(kKeyPattern, _shardKeyPattern)
            .append(kLowerBound, _lowerBoundKey)
            .append(kUpperBound, _upperBoundKey)
            .append(kForceSplit, _force);
        if (_maxSplitPoints) {
            commandBuilder.append(kMaxSplitPoints, _maxSplitPoints.get());
        }
        if (_maxChunkObjects) {
            commandBuilder.append(kMaxChunkObjects, _maxChunkObjects.get());
        }
        if (_maxChunkSizeBytes) {
            commandBuilder.append(kMaxChunkSizeBytes, _maxChunkSizeBytes.get());
        }
        return commandBuilder.obj();
    }

private:
    BSONObj _shardKeyPattern;
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    boost::optional<long long> _maxSplitPoints;
    boost::optional<long long> _maxChunkObjects;
    boost::optional<long long> _maxChunkSizeBytes;
    bool _force;


    static const std::string kCommandName;
    static const std::string kKeyPattern;
    static const std::string kLowerBound;
    static const std::string kUpperBound;
    static const std::string kMaxChunkSizeBytes;
    static const std::string kMaxSplitPoints;
    static const std::string kMaxChunkObjects;
    static const std::string kForceSplit;
};

class DataSizeCommandInfo : public CommandInfo {
public:
    DataSizeCommandInfo(const NamespaceString& nss,
                        const ShardId& shardId,
                        const BSONObj& shardKeyPattern,
                        const BSONObj& lowerBoundKey,
                        const BSONObj& upperBoundKey,
                        bool estimatedValue,
                        const ChunkVersion& version)
        : CommandInfo(shardId, nss),
          _shardKeyPattern(shardKeyPattern),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _estimatedValue(estimatedValue),
          _version(version) {}

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        commandBuilder.append(kCommandName, getNameSpace().toString())
            .append(kKeyPattern, _shardKeyPattern)
            .append(kMinValue, _lowerBoundKey)
            .append(kMaxValue, _upperBoundKey)
            .append(kEstimatedValue, _estimatedValue);

        _version.appendToCommand(&commandBuilder);

        return commandBuilder.obj();
    }

private:
    BSONObj _shardKeyPattern;
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    bool _estimatedValue;
    ChunkVersion _version;

    static const std::string kCommandName;
    static const std::string kKeyPattern;
    static const std::string kMinValue;
    static const std::string kMaxValue;
    static const std::string kEstimatedValue;
};

class SplitChunkCommandInfo : public CommandInfo {

public:
    SplitChunkCommandInfo(const NamespaceString& nss,
                          const ShardId& shardId,
                          const BSONObj& shardKeyPattern,
                          const BSONObj& lowerBoundKey,
                          const BSONObj& upperBoundKey,
                          const ChunkVersion& version,
                          const std::vector<BSONObj>& splitPoints)
        : CommandInfo(shardId, nss),
          _shardKeyPattern(shardKeyPattern),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _version(version),
          _splitPoints(splitPoints) {}

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        commandBuilder.append(kCommandName, getNameSpace().toString())
            .append(kShardName, getTarget().toString())
            .append(kKeyPattern, _shardKeyPattern)
            .append(kEpoch, _version.epoch())
            .append(kLowerBound, _lowerBoundKey)
            .append(kUpperBound, _upperBoundKey)
            .append(kSplitKeys, _splitPoints);
        return commandBuilder.obj();
    }

private:
    BSONObj _shardKeyPattern;
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    ChunkVersion _version;
    std::vector<BSONObj> _splitPoints;

    static const std::string kCommandName;
    static const std::string kShardName;
    static const std::string kKeyPattern;
    static const std::string kLowerBound;
    static const std::string kUpperBound;
    static const std::string kEpoch;
    static const std::string kSplitKeys;
};

class RecoveryCommandInfo : public CommandInfo {
public:
    RecoveryCommandInfo(const PersistedBalancerCommand& persistedCommand)
        : CommandInfo(persistedCommand.getTarget(), persistedCommand.getNss()),
          _serialisedCommand(persistedCommand.getRemoteCommand()),
          _requiresDistributedLock(persistedCommand.getRequiresDistributedLock()) {}

    BSONObj serialise() const override {
        return _serialisedCommand;
    }

    bool requiresRecoveryOnCrash() const override {
        return true;
    }

    bool requiresDistributedLock() const override {
        return _requiresDistributedLock;
    }

private:
    BSONObj _serialisedCommand;
    bool _requiresDistributedLock;
};

/**
 * Helper data structure for submitting the shard command associated to a Request to the
 * BalancerCommandsScheduler.
 */
struct CommandSubmissionHandle {
    CommandSubmissionHandle(UUID id, const std::shared_ptr<CommandInfo>& commandInfo)
        : id(id), commandInfo(commandInfo) {}

    CommandSubmissionHandle(CommandSubmissionHandle&& rhs)
        : id(rhs.id), commandInfo(std::move(rhs.commandInfo)) {}

    const UUID id;
    const std::shared_ptr<CommandInfo> commandInfo;
};


using ExecutionContext = executor::TaskExecutor::CallbackHandle;

/**
 * Helper data structure for storing the outcome of a Command submission.
 */
struct CommandSubmissionResult {
    CommandSubmissionResult(UUID id, bool acquiredDistLock, StatusWith<ExecutionContext>&& context)
        : id(id), acquiredDistLock(acquiredDistLock), context(std::move(context)) {}
    CommandSubmissionResult(CommandSubmissionResult&& rhs)
        : id(rhs.id), acquiredDistLock(rhs.acquiredDistLock), context(std::move(rhs.context)) {}
    CommandSubmissionResult(const CommandSubmissionResult& rhs) = delete;
    UUID id;
    bool acquiredDistLock;
    StatusWith<ExecutionContext> context;
};

/**
 * The class encapsulating all the properties supporting a request to BalancerCommandsSchedulerImpl
 * as it gets created, executed and completed/cancelled.
 * It offers helper methods to generate supporting classes to support the request processing.
 */
class RequestData {
public:
    RequestData(UUID id, std::shared_ptr<CommandInfo>&& commandInfo)
        : _id(id),
          _commandInfo(std::move(commandInfo)),
          _deferredResponse(std::make_shared<Notification<executor::RemoteCommandResponse>>()),
          _executionContext(boost::none) {
        invariant(_commandInfo);
    }

    RequestData(RequestData&& rhs)
        : _id(rhs._id),
          _commandInfo(std::move(rhs._commandInfo)),
          _deferredResponse(std::move(rhs._deferredResponse)),
          _executionContext(std::move(rhs._executionContext)) {}

    ~RequestData() = default;

    UUID getId() const {
        return _id;
    }

    const CommandInfo& getCommandInfo() const {
        return *_commandInfo;
    }

    CommandSubmissionHandle getSubmissionInfo() const {
        return CommandSubmissionHandle(_id, _commandInfo);
    }

    void addExecutionContext(ExecutionContext&& executionContext) {
        _executionContext = std::move(executionContext);
    }

    const boost::optional<executor::TaskExecutor::CallbackHandle>& getExecutionContext() {
        return _executionContext;
    }

    ResponseHandle getResponseHandle() {
        return ResponseHandle(_id, _deferredResponse);
    }

    void setOutcome(const executor::TaskExecutor::ResponseStatus& response) {
        _deferredResponse->set(response);
    }

private:
    RequestData& operator=(const RequestData& rhs) = delete;

    RequestData(const RequestData& rhs) = delete;

    const UUID _id;

    std::shared_ptr<CommandInfo> _commandInfo;

    std::shared_ptr<Notification<executor::RemoteCommandResponse>> _deferredResponse;

    boost::optional<ExecutionContext> _executionContext;
};

/**
 *  Implementation of BalancerCommandsScheduler, relying on the Notification library
 *  for the management of deferred response to remote commands.
 */
class BalancerCommandsSchedulerImpl : public BalancerCommandsScheduler {
public:
    BalancerCommandsSchedulerImpl();

    ~BalancerCommandsSchedulerImpl();

    void start(OperationContext* opCtx) override;

    void stop() override;

    std::unique_ptr<MoveChunkResponse> requestMoveChunk(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkType& chunk,
        const ShardId& destination,
        const MoveChunkSettings& commandSettings) override;

    std::unique_ptr<MergeChunksResponse> requestMergeChunks(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const ShardId& shardId,
                                                            const ChunkRange& chunkRange,
                                                            const ChunkVersion& version) override;

    std::unique_ptr<SplitVectorResponse> requestSplitVector(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkType& chunk,
        const KeyPattern& keyPattern,
        const SplitVectorSettings& commandSettings) override;

    std::unique_ptr<SplitChunkResponse> requestSplitChunk(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkType& chunk,
        const KeyPattern& keyPattern,
        const std::vector<BSONObj>& splitPoints) override;

    std::unique_ptr<ChunkDataSizeResponse> requestDataSize(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const ShardId& shardId,
                                                           const ChunkRange& chunkRange,
                                                           const ChunkVersion& version,
                                                           const KeyPattern& keyPattern,
                                                           bool estimatedValue) override;

private:
    enum class SchedulerState { Recovering, Running, Stopping, Stopped };

    static const int32_t _maxRunningRequests{10};

    Mutex _mutex = MONGO_MAKE_LATCH("BalancerCommandsSchedulerImpl::_mutex");
    Mutex _startStopMutex = MONGO_MAKE_LATCH("BalancerCommandsSchedulerImpl::_startStopMutex");

    SchedulerState _state{SchedulerState::Stopped};

    stdx::condition_variable _stateUpdatedCV;

    stdx::thread _workerThreadHandle;

    /**
     * Collection of all pending + running requests currently managed by
     * BalancerCommandsSchedulerImpl, organized by ID.
     */
    stdx::unordered_map<UUID, RequestData, UUID::Hash> _incompleteRequests;

    /**
     * List of request IDs that have not been yet submitted.
     */
    std::list<UUID> _pendingRequestIds;

    /**
     * List of request IDs that are currently running (submitted, but not yet completed).
     */
    stdx::unordered_set<UUID, UUID::Hash> _runningRequestIds;

    std::vector<UUID> _obsoleteRecoveryDocumentIds;

    /**
     * State to acquire and release DistLocks on a per namespace basis
     */
    struct Migrations {
        Migrations(DistLockManager::ScopedLock lock) : lock(std::move(lock)), numMigrations(1) {}

        DistLockManager::ScopedLock lock;
        int numMigrations;
    };
    stdx::unordered_map<NamespaceString, Migrations> _migrationLocks;

    ResponseHandle _buildAndEnqueueNewRequest(OperationContext* opCtx,
                                              std::shared_ptr<CommandInfo>&& commandInfo);

    ResponseHandle _enqueueRequest(WithLock, RequestData&& request);

    bool _canSubmitNewRequests(WithLock);

    Status _acquireDistLock(OperationContext* opCtx, NamespaceString nss);

    void _releaseDistLock(OperationContext* opCtx, NamespaceString nss);

    CommandSubmissionResult _submit(OperationContext* opCtx, const CommandSubmissionHandle& data);

    void _applySubmissionResult(WithLock,
                                OperationContext* opCtx,
                                CommandSubmissionResult&& submissionResult);

    void _applyCommandResponse(OperationContext* opCtx,
                               UUID requestId,
                               const executor::TaskExecutor::ResponseStatus& response);

    std::vector<RequestData> _loadRequestsToRecover(OperationContext* opCtx);

    void _cleanUpObsoleteRecoveryInfo(WithLock, OperationContext* opCtx);

    void _workerThread();
};

}  // namespace mongo

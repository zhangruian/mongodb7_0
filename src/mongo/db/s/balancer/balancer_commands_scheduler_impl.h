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

#include "mongo/db/s/balancer/balancer_command_document_gen.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_dist_locks.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Utility class to extract and hold information describing the remote client that submitted a
 * command.
 */
struct ExternalClientInfo {
    ExternalClientInfo(OperationContext* opCtx)
        : operationMetadata(opCtx), apiParameters(APIParameters::get(opCtx)) {}

    const ForwardableOperationMetadata operationMetadata;
    const APIParameters apiParameters;
};


/**
 * Base class describing the common traits of a shard command associated to a Request
 * received by BalancerCommandSchedulerImpl.
 */
class CommandInfo {
public:
    CommandInfo(const ShardId& targetShardId,
                const NamespaceString& nss,
                boost::optional<ExternalClientInfo>&& clientInfo)
        : _targetShardId(targetShardId), _nss(nss), _clientInfo(clientInfo) {}

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

    void attachOperationMetadataTo(OperationContext* opCtx) {
        if (_clientInfo) {
            _clientInfo.get().operationMetadata.setOn(opCtx);
        }
    }

    void appendCommandMetadataTo(BSONObjBuilder* commandBuilder) const {
        if (_clientInfo && _clientInfo.get().apiParameters.getParamsPassed()) {
            _clientInfo.get().apiParameters.appendInfo(commandBuilder);
        }
    }

private:
    ShardId _targetShardId;
    NamespaceString _nss;
    boost::optional<ExternalClientInfo> _clientInfo;
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
                         const ChunkVersion& version,
                         boost::optional<ExternalClientInfo>&& clientInfo)
        : CommandInfo(origin, nss, std::move(clientInfo)),
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
        appendCommandMetadataTo(&commandBuilder);
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
        : CommandInfo(shardId, nss, boost::none),
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
        : CommandInfo(shardId, nss, boost::none),
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
        : CommandInfo(shardId, nss, boost::none),
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
        : CommandInfo(shardId, nss, boost::none),
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
        : CommandInfo(persistedCommand.getTarget(), persistedCommand.getNss(), boost::none),
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
 * Helper data structure for submitting the remote command associated to a BalancerCommandsScheduler
 * Request.
 */
struct CommandSubmissionParameters {
    CommandSubmissionParameters(UUID id, const std::shared_ptr<CommandInfo>& commandInfo)
        : id(id), commandInfo(commandInfo) {}

    CommandSubmissionParameters(CommandSubmissionParameters&& rhs)
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
 */
class RequestData {
public:
    RequestData(UUID id, std::shared_ptr<CommandInfo>&& commandInfo)
        : _id(id),
          _completedOrAborted(false),
          _holdingDistLock(false),
          _commandInfo(std::move(commandInfo)),
          _responsePromise{NonNullPromiseTag{}},
          _executionContext(boost::none) {
        invariant(_commandInfo);
    }

    RequestData(RequestData&& rhs)
        : _id(rhs._id),
          _completedOrAborted(rhs._completedOrAborted),
          _holdingDistLock(rhs._holdingDistLock),
          _commandInfo(std::move(rhs._commandInfo)),
          _responsePromise(std::move(rhs._responsePromise)),
          _executionContext(std::move(rhs._executionContext)) {}

    ~RequestData() = default;

    UUID getId() const {
        return _id;
    }

    CommandSubmissionParameters getSubmissionParameters() const {
        return CommandSubmissionParameters(_id, _commandInfo);
    }

    Status applySubmissionResult(CommandSubmissionResult&& submissionResult) {
        invariant(_id == submissionResult.id);
        _holdingDistLock = submissionResult.acquiredDistLock;
        if (_completedOrAborted) {
            // A remote response was already received by the time the submission gets processed.
            // Keep the original outcome and continue the workflow.
            return Status::OK();
        }
        auto submissionStatus = submissionResult.context.getStatus();
        if (submissionStatus.isOK()) {
            // store the execution context to be able to serve future cancel requests.
            _executionContext = std::move(submissionResult.context.getValue());
        } else {
            // cascade the submission failure
            setOutcome(submissionStatus);
        }
        return submissionStatus;
    }

    const boost::optional<executor::TaskExecutor::CallbackHandle>& getExecutionContext() {
        return _executionContext;
    }

    const NamespaceString& getNamespace() const {
        return _commandInfo->getNameSpace();
    }

    bool holdsDistributedLock() const {
        return _holdingDistLock;
    }

    bool isRecoverable() const {
        return _commandInfo->requiresRecoveryOnCrash();
    }

    Future<executor::RemoteCommandResponse> getOutcomeFuture() {
        return _responsePromise.getFuture();
    }

    void setOutcome(const StatusWith<executor::RemoteCommandResponse>& response) {
        _responsePromise.setFrom(response);
        _completedOrAborted = true;
    }

private:
    RequestData& operator=(const RequestData& rhs) = delete;

    RequestData(const RequestData& rhs) = delete;

    const UUID _id;

    bool _completedOrAborted;

    bool _holdingDistLock;

    std::shared_ptr<CommandInfo> _commandInfo;

    Promise<executor::RemoteCommandResponse> _responsePromise;

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

    SemiFuture<void> requestMoveChunk(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const ChunkType& chunk,
                                      const ShardId& destination,
                                      const MoveChunkSettings& commandSettings,
                                      bool issuedByRemoteUser) override;

    SemiFuture<void> requestMergeChunks(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const ChunkRange& chunkRange,
                                        const ChunkVersion& version) override;

    SemiFuture<std::vector<BSONObj>> requestSplitVector(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkType& chunk,
        const KeyPattern& keyPattern,
        const SplitVectorSettings& commandSettings) override;

    SemiFuture<void> requestSplitChunk(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const ChunkType& chunk,
                                       const KeyPattern& keyPattern,
                                       const std::vector<BSONObj>& splitPoints) override;

    SemiFuture<DataSizeResponse> requestDataSize(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const ShardId& shardId,
                                                 const ChunkRange& chunkRange,
                                                 const ChunkVersion& version,
                                                 const KeyPattern& keyPattern,
                                                 bool estimatedValue) override;

private:
    enum class SchedulerState { Recovering, Running, Stopping, Stopped };

    std::shared_ptr<executor::TaskExecutor> _executor{nullptr};

    // Protects the in-memory state of the Scheduler
    // (_state, _requests, _unsubmittedRequestIds, _recentlyCompletedRequests).
    Mutex _mutex = MONGO_MAKE_LATCH("BalancerCommandsSchedulerImpl::_mutex");

    SchedulerState _state{SchedulerState::Stopped};

    stdx::condition_variable _stateUpdatedCV;

    stdx::thread _workerThreadHandle;

    /**
     * List of all unsubmitted + submitted + completed, but not cleaned up yet requests managed by
     * BalancerCommandsSchedulerImpl, organized by ID.
     */
    stdx::unordered_map<UUID, RequestData, UUID::Hash> _requests;

    /**
     * List of request IDs that have not been yet submitted for remote execution.
     */
    std::vector<UUID> _unsubmittedRequestIds;

    /**
     * List of completed/cancelled requests IDs that may still hold synchronisation resources or
     * persisted state that the scheduler needs to release/clean up.
     */
    std::vector<UUID> _recentlyCompletedRequestIds;

    /**
     * Centralised accessor for all the distributed locks required by the Scheduler.
     * Only _workerThread() is supposed to interact with this class.
     */
    BalancerDistLocks _distributedLocks;

    /*
     * Counter of oustanding requests that were interrupted by a prior step-down/crash event,
     * and that the scheduler is currently submitting as part of its initial recovery phase.
     */
    size_t _numRequestsToRecover{0};

    Future<executor::RemoteCommandResponse> _buildAndEnqueueNewRequest(
        OperationContext* opCtx, std::shared_ptr<CommandInfo>&& commandInfo);

    void _enqueueRequest(WithLock, RequestData&& request);

    void _performDeferredCleanup(OperationContext* opCtx,
                                 std::vector<RequestData>&& requestsHoldingResources);

    CommandSubmissionResult _submit(OperationContext* opCtx,
                                    const CommandSubmissionParameters& data);

    void _applySubmissionResult(WithLock, CommandSubmissionResult&& submissionResult);

    void _applyCommandResponse(UUID requestId, const executor::RemoteCommandResponse& response);

    std::vector<RequestData> _loadRequestsToRecover(OperationContext* opCtx);

    void _workerThread();
};

}  // namespace mongo

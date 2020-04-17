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
#include "mongo/client/server_is_master_monitor.h"

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(overrideMaxAwaitTimeMS);

const BSONObj IS_MASTER_BSON = BSON("isMaster" << 1);

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;

const Milliseconds kZeroMs = Milliseconds{0};
}  // namespace

SingleServerIsMasterMonitor::SingleServerIsMasterMonitor(
    const MongoURI& setUri,
    const sdam::ServerAddress& host,
    boost::optional<TopologyVersion> topologyVersion,
    Milliseconds heartbeatFrequencyMS,
    sdam::TopologyEventsPublisherPtr eventListener,
    std::shared_ptr<executor::TaskExecutor> executor)
    : _host(host),
      _topologyVersion(topologyVersion),
      _eventListener(eventListener),
      _executor(executor),
      _heartbeatFrequencyMS(_overrideRefreshPeriod(heartbeatFrequencyMS)),
      _isExpedited(true),
      _isShutdown(true),
      _setUri(setUri) {
    LOGV2_DEBUG(4333217,
                kLogLevel + 1,
                "RSM {setName} monitoring {host}",
                "host"_attr = host,
                "setName"_attr = _setUri.getSetName());
}

void SingleServerIsMasterMonitor::init() {
    stdx::lock_guard lock(_mutex);
    _isShutdown = false;
    _scheduleNextIsMaster(lock, Milliseconds(0));
}

void SingleServerIsMasterMonitor::requestImmediateCheck() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    // The previous refresh period may or may not have been expedited.
    // Saving the value here before we change to expedited mode.
    const auto previousRefreshPeriod = _currentRefreshPeriod(lock, false);

    if (!_isExpedited) {
        // save some log lines.
        LOGV2_DEBUG(4333227,
                    kLogLevel,
                    "RSM {setName} monitoring {host} in expedited mode until we detect a primary.",
                    "host"_attr = _host,
                    "setName"_attr = _setUri.getSetName());

        // This will change the _currentRefreshPeriod to the shorter expedited duration.
        _isExpedited = true;
    }

    // Get the new expedited refresh period.
    const auto expeditedRefreshPeriod = _currentRefreshPeriod(lock, false);

    if (_isMasterOutstanding) {
        LOGV2_DEBUG(4333216,
                    kLogLevel + 2,
                    "RSM {setName} immediate isMaster check requested, but there "
                    "is already an outstanding request.",
                    "setName"_attr = _setUri.getSetName());
        return;
    }

    if (const auto maybeDelayUntilNextCheck = calculateExpeditedDelayUntilNextCheck(
            _timeSinceLastCheck(), expeditedRefreshPeriod, previousRefreshPeriod)) {
        _rescheduleNextIsMaster(lock, *maybeDelayUntilNextCheck);
    }
}

boost::optional<Milliseconds> SingleServerIsMasterMonitor::calculateExpeditedDelayUntilNextCheck(
    const boost::optional<Milliseconds>& maybeTimeSinceLastCheck,
    const Milliseconds& expeditedRefreshPeriod,
    const Milliseconds& previousRefreshPeriod) {
    invariant(expeditedRefreshPeriod.count() <= previousRefreshPeriod.count());

    const auto timeSinceLastCheck =
        (maybeTimeSinceLastCheck) ? *maybeTimeSinceLastCheck : Milliseconds::max();
    invariant(timeSinceLastCheck.count() >= 0);

    if (timeSinceLastCheck == previousRefreshPeriod)
        return boost::none;

    if (timeSinceLastCheck > expeditedRefreshPeriod)
        return Milliseconds(0);

    const auto delayUntilExistingRequest = previousRefreshPeriod - timeSinceLastCheck;

    // Calculate when the next isMaster should be scheduled.
    const Milliseconds delayUntilNextCheck = expeditedRefreshPeriod - timeSinceLastCheck;

    // Do nothing if the time would be greater-than or equal to the existing request.
    return (delayUntilNextCheck >= delayUntilExistingRequest)
        ? boost::none
        : boost::optional<Milliseconds>(delayUntilNextCheck);
}

boost::optional<Milliseconds> SingleServerIsMasterMonitor::_timeSinceLastCheck() const {
    return (_lastIsMasterAt) ? boost::optional<Milliseconds>(_executor->now() - *_lastIsMasterAt)
                             : boost::none;
}

void SingleServerIsMasterMonitor::_rescheduleNextIsMaster(WithLock lock, Milliseconds delay) {
    LOGV2_DEBUG(4333218,
                kLogLevel,
                "Rescheduling the next replica set monitoring request",
                "setName"_attr = _setUri.getSetName(),
                "host"_attr = _host,
                "duration"_attr = delay);
    _cancelOutstandingRequest(lock);
    _scheduleNextIsMaster(lock, delay);
}

void SingleServerIsMasterMonitor::_scheduleNextIsMaster(WithLock, Milliseconds delay) {
    if (_isShutdown)
        return;

    invariant(!_isMasterOutstanding);

    auto swCbHandle = _executor->scheduleWorkAt(
        _executor->now() + delay,
        [self = shared_from_this()](const executor::TaskExecutor::CallbackArgs& cbData) {
            if (!cbData.status.isOK()) {
                return;
            }
            self->_doRemoteCommand();
        });

    if (!swCbHandle.isOK()) {
        _onIsMasterFailure(swCbHandle.getStatus(), BSONObj());
        return;
    }

    _nextIsMasterHandle = swCbHandle.getValue();
}

void SingleServerIsMasterMonitor::_doRemoteCommand() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    StatusWith<executor::TaskExecutor::CallbackHandle> swCbHandle = [&]() {
        if (_topologyVersion) {
            return _scheduleStreamableIsMaster();
        }

        return _scheduleSingleIsMaster();
    }();

    if (!swCbHandle.isOK()) {
        _onIsMasterFailure(swCbHandle.getStatus(), BSONObj());
        uasserted(46156012, swCbHandle.getStatus().toString());
    }

    _isMasterOutstanding = true;
    _remoteCommandHandle = swCbHandle.getValue();
}

StatusWith<TaskExecutor::CallbackHandle>
SingleServerIsMasterMonitor::_scheduleStreamableIsMaster() {
    auto maxAwaitTimeMS = durationCount<Milliseconds>(kMaxAwaitTimeMs);
    overrideMaxAwaitTimeMS.execute([&](const BSONObj& data) {
        maxAwaitTimeMS =
            durationCount<Milliseconds>(Milliseconds(data["maxAwaitTimeMS"].numberInt()));
    });

    auto isMasterCmd = BSON("isMaster" << 1 << "maxAwaitTimeMS" << maxAwaitTimeMS
                                       << "topologyVersion" << _topologyVersion->toBSON());

    _timeoutMS = SdamConfiguration::kDefaultConnectTimeoutMS + kMaxAwaitTimeMs;
    auto request = executor::RemoteCommandRequest(
        HostAndPort(_host), "admin", isMasterCmd, nullptr, _timeoutMS);
    request.sslMode = _setUri.getSSLMode();

    auto swCbHandle = _executor->scheduleExhaustRemoteCommand(
        std::move(request),
        [self = shared_from_this()](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            Milliseconds nextRefreshPeriod;
            {
                stdx::lock_guard lk(self->_mutex);

                if (self->_isShutdown) {
                    self->_isMasterOutstanding = false;
                    LOGV2_DEBUG(4495400,
                                kLogLevel,
                                "RSM {setName} not processing response: {status}",
                                "status"_attr = result.response.status,
                                "setName"_attr = self->_setUri.getSetName());
                    return;
                }

                auto responseTopologyVersion = result.response.data.getField("topologyVersion");
                if (responseTopologyVersion) {
                    self->_topologyVersion = TopologyVersion::parse(
                        IDLParserErrorContext("TopologyVersion"), responseTopologyVersion.Obj());
                } else {
                    self->_topologyVersion = boost::none;
                }

                self->_lastIsMasterAt = self->_executor->now();
                if (!result.response.isOK() || !result.response.moreToCome) {
                    self->_isMasterOutstanding = false;
                    nextRefreshPeriod = self->_currentRefreshPeriod(lk, result.response.isOK());
                    self->_scheduleNextIsMaster(lk, nextRefreshPeriod);
                }
            }

            if (result.response.isOK()) {
                self->_onIsMasterSuccess(result.response.data);
            } else {
                self->_onIsMasterFailure(result.response.status, result.response.data);
            }
        });

    return swCbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> SingleServerIsMasterMonitor::_scheduleSingleIsMaster() {
    _timeoutMS = SdamConfiguration::kDefaultConnectTimeoutMS;
    auto request = executor::RemoteCommandRequest(
        HostAndPort(_host), "admin", IS_MASTER_BSON, nullptr, _timeoutMS);
    request.sslMode = _setUri.getSSLMode();

    auto swCbHandle = _executor->scheduleRemoteCommand(
        std::move(request),
        [self = shared_from_this()](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            Milliseconds nextRefreshPeriod;
            {
                stdx::lock_guard lk(self->_mutex);
                self->_isMasterOutstanding = false;

                if (self->_isShutdown) {
                    LOGV2_DEBUG(4333219,
                                kLogLevel,
                                "RSM {setName} not processing response: {status}",
                                "status"_attr = result.response.status,
                                "setName"_attr = self->_setUri.getSetName());
                    return;
                }

                self->_lastIsMasterAt = self->_executor->now();

                auto responseTopologyVersion = result.response.data.getField("topologyVersion");
                if (responseTopologyVersion) {
                    self->_topologyVersion = TopologyVersion::parse(
                        IDLParserErrorContext("TopologyVersion"), responseTopologyVersion.Obj());
                } else {
                    self->_topologyVersion = boost::none;
                }

                if (!result.response.isOK() || !result.response.moreToCome) {
                    self->_isMasterOutstanding = false;
                    nextRefreshPeriod = self->_currentRefreshPeriod(lk, result.response.isOK());
                    self->_scheduleNextIsMaster(lk, nextRefreshPeriod);
                }
            }

            if (result.response.isOK()) {
                self->_onIsMasterSuccess(result.response.data);
            } else {
                self->_onIsMasterFailure(result.response.status, result.response.data);
            }
        });

    return swCbHandle;
}

void SingleServerIsMasterMonitor::shutdown() {
    stdx::lock_guard lock(_mutex);
    if (std::exchange(_isShutdown, true)) {
        return;
    }

    LOGV2_DEBUG(4333220,
                kLogLevel + 1,
                "RSM {setName} Closing host {host}",
                "host"_attr = _host,
                "setName"_attr = _setUri.getSetName());

    _cancelOutstandingRequest(lock);

    _executor = nullptr;

    LOGV2_DEBUG(4333229,
                kLogLevel + 1,
                "RSM {setName} Done Closing host {host}",
                "host"_attr = _host,
                "setName"_attr = _setUri.getSetName());
}

void SingleServerIsMasterMonitor::_cancelOutstandingRequest(WithLock) {
    if (_remoteCommandHandle) {
        _executor->cancel(_remoteCommandHandle);
    }

    if (_nextIsMasterHandle) {
        _executor->cancel(_nextIsMasterHandle);
    }

    _isMasterOutstanding = false;
}

void SingleServerIsMasterMonitor::_onIsMasterSuccess(const BSONObj bson) {
    LOGV2_DEBUG(4333221,
                kLogLevel + 1,
                "RSM {setName} received successful isMaster for server {host}: {bson}",
                "host"_attr = _host,
                "setName"_attr = _setUri.getSetName(),
                "bson"_attr = bson.toString());

    _eventListener->onServerHeartbeatSucceededEvent(_host, bson);
}

void SingleServerIsMasterMonitor::_onIsMasterFailure(const Status& status, const BSONObj bson) {
    LOGV2_DEBUG(4333222,
                kLogLevel,
                "RSM {setName} received failed isMaster for server {host}: {status}: {bson}",
                "host"_attr = _host,
                "status"_attr = status.toString(),
                "setName"_attr = _setUri.getSetName(),
                "bson"_attr = bson.toString());

    _eventListener->onServerHeartbeatFailureEvent(status, _host, bson);
}

Milliseconds SingleServerIsMasterMonitor::_overrideRefreshPeriod(Milliseconds original) {
    Milliseconds r = original;
    static constexpr auto kPeriodField = "period"_sd;
    if (auto modifyReplicaSetMonitorDefaultRefreshPeriod =
            globalFailPointRegistry().find("modifyReplicaSetMonitorDefaultRefreshPeriod")) {
        modifyReplicaSetMonitorDefaultRefreshPeriod->executeIf(
            [&r](const BSONObj& data) {
                r = duration_cast<Milliseconds>(Seconds{data.getIntField(kPeriodField)});
            },
            [](const BSONObj& data) { return data.hasField(kPeriodField); });
    }
    return r;
}

Milliseconds SingleServerIsMasterMonitor::_currentRefreshPeriod(WithLock,
                                                                bool scheduleImmediately) {
    if (scheduleImmediately)
        return Milliseconds(0);

    return (_isExpedited) ? sdam::SdamConfiguration::kMinHeartbeatFrequencyMS
                          : _heartbeatFrequencyMS;
}

void SingleServerIsMasterMonitor::disableExpeditedChecking() {
    stdx::lock_guard lock(_mutex);
    _isExpedited = false;
}


ServerIsMasterMonitor::ServerIsMasterMonitor(
    const MongoURI& setUri,
    const sdam::SdamConfiguration& sdamConfiguration,
    sdam::TopologyEventsPublisherPtr eventsPublisher,
    sdam::TopologyDescriptionPtr initialTopologyDescription,
    std::shared_ptr<executor::TaskExecutor> executor)
    : _sdamConfiguration(sdamConfiguration),
      _eventPublisher(eventsPublisher),
      _executor(_setupExecutor(executor)),
      _isShutdown(false),
      _setUri(setUri) {
    LOGV2_DEBUG(4333223,
                kLogLevel,
                "RSM {setName} monitoring {size} members.",
                "setName"_attr = _setUri.getSetName(),
                "size"_attr = initialTopologyDescription->getServers().size());
    onTopologyDescriptionChangedEvent(
        initialTopologyDescription->getId(), nullptr, initialTopologyDescription);
}

void ServerIsMasterMonitor::shutdown() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    _isShutdown = true;
    for (auto singleMonitor : _singleMonitors) {
        singleMonitor.second->shutdown();
    }
}

void ServerIsMasterMonitor::onTopologyDescriptionChangedEvent(
    UUID topologyId,
    sdam::TopologyDescriptionPtr previousDescription,
    sdam::TopologyDescriptionPtr newDescription) {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    const auto newType = newDescription->getType();
    using sdam::TopologyType;

    if (newType == TopologyType::kSingle || newType == TopologyType::kReplicaSetWithPrimary ||
        newType == TopologyType::kSharded) {
        _disableExpeditedChecking(lock);
    }

    // remove monitors that are missing from the topology
    auto it = _singleMonitors.begin();
    while (it != _singleMonitors.end()) {
        const auto& serverAddress = it->first;
        if (newDescription->findServerByAddress(serverAddress) == boost::none) {
            auto& singleMonitor = _singleMonitors[serverAddress];
            singleMonitor->shutdown();
            LOGV2_DEBUG(4333225,
                        kLogLevel,
                        "RSM {setName} host {addr} was removed from the topology.",
                        "setName"_attr = _setUri.getSetName(),
                        "addr"_attr = serverAddress);
            it = _singleMonitors.erase(it, ++it);
        } else {
            ++it;
        }
    }

    // add new monitors
    newDescription->findServers([this](const sdam::ServerDescriptionPtr& serverDescription) {
        const auto& serverAddress = serverDescription->getAddress();
        bool isMissing =
            _singleMonitors.find(serverDescription->getAddress()) == _singleMonitors.end();
        if (isMissing) {
            LOGV2_DEBUG(4333226,
                        kLogLevel,
                        "RSM {setName} {addr} was added to the topology.",
                        "setName"_attr = _setUri.getSetName(),
                        "addr"_attr = serverAddress);
            _singleMonitors[serverAddress] = std::make_shared<SingleServerIsMasterMonitor>(
                _setUri,
                serverAddress,
                serverDescription->getTopologyVersion(),
                _sdamConfiguration.getHeartBeatFrequency(),
                _eventPublisher,
                _executor);
            _singleMonitors[serverAddress]->init();
        }
        return isMissing;
    });
}

std::shared_ptr<executor::TaskExecutor> ServerIsMasterMonitor::_setupExecutor(
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    if (executor)
        return executor;

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    auto net = executor::makeNetworkInterface(
        "ServerIsMasterMonitor-TaskExecutor", nullptr, std::move(hookList));
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto result = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    result->startup();
    return result;
}

void ServerIsMasterMonitor::requestImmediateCheck() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    for (auto& addressAndMonitor : _singleMonitors) {
        addressAndMonitor.second->requestImmediateCheck();
    }
}

void ServerIsMasterMonitor::_disableExpeditedChecking(WithLock) {
    for (auto& addressAndMonitor : _singleMonitors) {
        addressAndMonitor.second->disableExpeditedChecking();
    }
}
}  // namespace mongo

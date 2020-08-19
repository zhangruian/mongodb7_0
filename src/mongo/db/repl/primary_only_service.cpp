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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/primary_only_service.h"

#include <utility>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceHangBeforeRebuildingInstances);
MONGO_FAIL_POINT_DEFINE(PrimaryOnlyServiceFailRebuildingInstances);

namespace {
const auto _registryDecoration = ServiceContext::declareDecoration<PrimaryOnlyServiceRegistry>();

const auto _registryRegisterer =
    ReplicaSetAwareServiceRegistry::Registerer<PrimaryOnlyServiceRegistry>(
        "PrimaryOnlyServiceRegistry");

const Status kExecutorShutdownStatus(ErrorCodes::InterruptedDueToReplStateChange,
                                     "PrimaryOnlyService executor shut down due to stepDown");
}  // namespace

PrimaryOnlyServiceRegistry* PrimaryOnlyServiceRegistry::get(ServiceContext* serviceContext) {
    return &_registryDecoration(serviceContext);
}

void PrimaryOnlyServiceRegistry::registerService(std::unique_ptr<PrimaryOnlyService> service) {
    auto ns = service->getStateDocumentsNS();
    auto name = service->getServiceName();
    auto servicePtr = service.get();

    auto [_, inserted] = _servicesByName.emplace(name, std::move(service));
    invariant(inserted,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") that is already registered");

    auto [existingServiceIt, inserted2] = _servicesByNamespace.emplace(ns.toString(), servicePtr);
    auto existingService = existingServiceIt->second;
    invariant(inserted2,
              str::stream() << "Attempted to register PrimaryOnlyService (" << name
                            << ") with state document namespace \"" << ns
                            << "\" that is already in use by service "
                            << existingService->getServiceName());
}

PrimaryOnlyService* PrimaryOnlyServiceRegistry::lookupServiceByName(StringData serviceName) {
    auto it = _servicesByName.find(serviceName);
    invariant(it != _servicesByName.end());
    auto servicePtr = it->second.get();
    invariant(servicePtr);
    return servicePtr;
}

PrimaryOnlyService* PrimaryOnlyServiceRegistry::lookupServiceByNamespace(
    const NamespaceString& ns) {
    auto it = _servicesByNamespace.find(ns.toString());
    if (it == _servicesByNamespace.end()) {
        return nullptr;
    }
    auto servicePtr = it->second;
    invariant(servicePtr);
    return servicePtr;
}

void PrimaryOnlyServiceRegistry::onStartup(OperationContext* opCtx) {
    for (auto& service : _servicesByName) {
        service.second->startup(opCtx);
    }
}

void PrimaryOnlyServiceRegistry::onStepUpComplete(OperationContext* opCtx, long long term) {
    const auto stepUpOpTime = ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime();
    invariant(term == stepUpOpTime.getTerm(),
              str::stream() << "Term from last optime (" << stepUpOpTime.getTerm()
                            << ") doesn't match the term we're stepping up in (" << term << ")");

    for (auto& service : _servicesByName) {
        service.second->onStepUp(stepUpOpTime);
    }
}

void PrimaryOnlyServiceRegistry::onStepDown() {
    for (auto& service : _servicesByName) {
        service.second->onStepDown();
    }
}

void PrimaryOnlyServiceRegistry::shutdown() {
    for (auto& service : _servicesByName) {
        service.second->shutdown();
    }
}

PrimaryOnlyService::PrimaryOnlyService(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

void PrimaryOnlyService::startup(OperationContext* opCtx) {
    // Initialize the thread pool options with the service-specific limits on pool size.
    ThreadPool::Options threadPoolOptions(getThreadPoolLimits());

    // Now add the options that are fixed for all PrimaryOnlyServices.
    threadPoolOptions.threadNamePrefix = getServiceName() + "-";
    threadPoolOptions.poolName = getServiceName() + "ThreadPool";
    threadPoolOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());
    };

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));
    _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(threadPoolOptions),
        executor::makeNetworkInterface(getServiceName() + "Network", nullptr, std::move(hookList)));
    _executor->startup();
}

void PrimaryOnlyService::onStepUp(const OpTime& stepUpOpTime) {
    InstanceMap savedInstances;
    auto newThenOldScopedExecutor =
        std::make_shared<executor::ScopedTaskExecutor>(_executor, kExecutorShutdownStatus);
    {
        stdx::lock_guard lk(_mutex);

        auto newTerm = stepUpOpTime.getTerm();
        invariant(newTerm > _term,
                  str::stream() << "term " << newTerm << " is not greater than " << _term);
        _term = newTerm;
        _state = State::kRebuilding;

        // Install a new executor, while moving the old one into 'newThenOldScopedExecutor' so it
        // can be accessed outside of _mutex.
        using std::swap;
        swap(newThenOldScopedExecutor, _scopedExecutor);
        // Don't destroy the Instances until all outstanding tasks run against them are complete.
        swap(savedInstances, _instances);
    }

    // Ensure that all tasks from the previous term have completed before allowing tasks to be
    // scheduled on the new executor.
    if (newThenOldScopedExecutor) {
        // Shutdown happens in onStepDown of previous term, so we only need to join() here.
        (*newThenOldScopedExecutor)->join();
    }

    // Now wait for the first write of the new term to be majority committed, so that we know all
    // previous writes to state documents are also committed, and then schedule work to rebuild
    // Instances from their persisted state documents.
    stdx::lock_guard lk(_mutex);
    WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(stepUpOpTime)
        .thenRunOn(**_scopedExecutor)
        .then([this] { _rebuildInstances(); })
        .getAsync([](auto&&) {});  // Ignore the result Future
}

void PrimaryOnlyService::onStepDown() {
    stdx::lock_guard lk(_mutex);

    if (_scopedExecutor) {
        (*_scopedExecutor)->shutdown();
    }
    _state = State::kPaused;
    _rebuildStatus = Status::OK();
}

void PrimaryOnlyService::shutdown() {
    InstanceMap savedInstances;
    std::shared_ptr<executor::TaskExecutor> savedExecutor;

    {
        stdx::lock_guard lk(_mutex);

        // Save the executor to join() with it outside of _mutex.
        using std::swap;
        swap(savedExecutor, _executor);
        // Maintain the lifetime of the instances until all outstanding tasks using them are
        // complete.
        swap(savedInstances, _instances);

        _scopedExecutor.reset();
        _state = State::kShutdown;
    }

    if (savedExecutor) {
        savedExecutor->shutdown();
        savedExecutor->join();
    }
    savedInstances.clear();
}

std::shared_ptr<PrimaryOnlyService::Instance> PrimaryOnlyService::getOrCreateInstance(
    BSONObj initialState) {
    const auto idElem = initialState["_id"];
    uassert(4908702,
            str::stream() << "Missing _id element when adding new instance of PrimaryOnlyService \""
                          << getServiceName() << "\"",
            !idElem.eoo());
    InstanceID instanceID = idElem.wrap();

    stdx::unique_lock lk(_mutex);
    while (_state == State::kRebuilding) {
        _rebuildCV.wait(lk);
    }
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    uassert(
        ErrorCodes::NotMaster,
        str::stream() << "Not Primary when trying to create a new instance of PrimaryOnlyService "
                      << getServiceName(),
        _state == State::kRunning);

    auto it = _instances.find(instanceID);
    if (it != _instances.end()) {
        return it->second;
    }
    auto [it2, inserted] = _instances.emplace(std::move(instanceID).getOwned(),
                                              constructInstance(std::move(initialState)));
    invariant(inserted);

    // Kick off async work to run the instance
    it2->second->scheduleRun(_scopedExecutor);

    return it2->second;
}

boost::optional<std::shared_ptr<PrimaryOnlyService::Instance>> PrimaryOnlyService::lookupInstance(
    const InstanceID& id) {
    stdx::unique_lock lk(_mutex);
    while (_state == State::kRebuilding) {
        _rebuildCV.wait(lk);
    }
    if (_state == State::kShutdown || _state == State::kPaused) {
        invariant(_instances.empty());
        return boost::none;
    }
    if (_state == State::kRebuildFailed) {
        uassertStatusOK(_rebuildStatus);
    }
    invariant(_state == State::kRunning);


    auto it = _instances.find(id);
    if (it == _instances.end()) {
        return boost::none;
    }

    return it->second;
}

void PrimaryOnlyService::releaseInstance(const InstanceID& id) {
    stdx::lock_guard lk(_mutex);
    _instances.erase(id);
}

void PrimaryOnlyService::releaseAllInstances() {
    stdx::lock_guard lk(_mutex);
    _instances.clear();
}

void PrimaryOnlyService::_rebuildInstances() noexcept {
    std::vector<BSONObj> stateDocuments;
    {
        auto opCtx = cc().makeOperationContext();
        DBDirectClient client(opCtx.get());
        try {
            if (MONGO_unlikely(PrimaryOnlyServiceFailRebuildingInstances.shouldFail())) {
                uassertStatusOK(
                    Status(ErrorCodes::InternalError, "Querying state documents failed"));
            }

            auto cursor = client.query(getStateDocumentsNS(), Query());
            while (cursor->more()) {
                stateDocuments.push_back(cursor->nextSafe().getOwned());
            }
        } catch (const DBException& e) {
            LOGV2_ERROR(4923601,
                        "Failed to start PrimaryOnlyService {service} because the query on {ns} "
                        "for state documents failed due to {error}",
                        "ns"_attr = getStateDocumentsNS(),
                        "service"_attr = getServiceName(),
                        "error"_attr = e);

            Status status = e.toStatus();
            status.addContext(str::stream()
                              << "Failed to start PrimaryOnlyService \"" << getServiceName()
                              << "\" because the query for state documents on ns \""
                              << getStateDocumentsNS() << "\" failed");

            stdx::lock_guard lk(_mutex);
            _state = State::kRebuildFailed;
            _rebuildStatus = std::move(status);
            _rebuildCV.notify_all();
            return;
        }
    }

    if (MONGO_unlikely(PrimaryOnlyServiceHangBeforeRebuildingInstances.shouldFail())) {
        PrimaryOnlyServiceHangBeforeRebuildingInstances.pauseWhileSet();
    }

    stdx::lock_guard lk(_mutex);
    if (_state != State::kRebuilding) {
        // Node stepped down before finishing rebuilding service from previous stepUp.
        _rebuildCV.notify_all();
        return;
    }
    invariant(_instances.empty());

    for (auto&& doc : stateDocuments) {
        auto idElem = doc["_id"];
        fassert(4923602, !idElem.eoo());
        auto instanceID = idElem.wrap().getOwned();
        auto instance = constructInstance(std::move(doc));

        auto [_, inserted] = _instances.emplace(instanceID, instance);
        invariant(inserted);
        instance->scheduleRun(_scopedExecutor);
    }
    _state = State::kRunning;
    _rebuildCV.notify_all();
}

void PrimaryOnlyService::Instance::scheduleRun(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    invariant(!_running);
    _running = true;

    (*executor)->schedule([this, executor = std::move(executor)](auto status) {
        if (ErrorCodes::isCancelationError(status) || ErrorCodes::NotMaster == status) {
            return;
        }
        invariant(status);

        run(std::move(executor));
    });
}

}  // namespace repl
}  // namespace mongo

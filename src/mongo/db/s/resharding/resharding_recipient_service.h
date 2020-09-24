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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"

namespace mongo {
constexpr StringData kReshardingRecipientServiceName = "ReshardingRecipientService"_sd;

class ReshardingRecipientService final : public repl::PrimaryOnlyService {
public:
    explicit ReshardingRecipientService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingRecipientService() = default;

    StringData getServiceName() const override {
        return kReshardingRecipientServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kRecipientReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingRecipientService thread pool.
        return ThreadPool::Limits();
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override;
};

/**
 * Represents the current state of a resharding recipient operation on this shard. This class
 * drives state transitions and updates to underlying on-disk metadata.
 */
class RecipientStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine> {
public:
    explicit RecipientStateMachine(const BSONObj& recipientDoc);

    void run(std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept override;

    void interrupt(Status status) override{};

    /**
     * TODO(SERVER-51021) Report ReshardingRecipientService Instances in currentOp().
     */
    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final {
        return boost::none;
    }

    void onReshardingFieldsChanges(
        boost::optional<TypeCollectionReshardingFields> reshardingFields);

private:
    // The following functions correspond to the actions to take at a particular recipient state.
    void _createTemporaryReshardingCollectionThenTransitionToInitialized();

    ExecutorFuture<void> _awaitAllDonorsPreparedToDonateThenTransitionToCloning(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    void _cloneThenTransitionToApplying();

    void _applyThenTransitionToSteadyState();

    ExecutorFuture<void> _awaitAllDonorsMirroringThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    ExecutorFuture<void> _awaitCoordinatorHasCommittedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    void _renameTemporaryReshardingCollectionThenDeleteLocalState();

    void _fulfillAllDonorsPreparedToDonate(Timestamp);

    // Transitions the state on-disk and in-memory to 'endState'.
    void _transitionState(RecipientStateEnum endState,
                          boost::optional<Timestamp> fetchTimestamp = boost::none);

    // Transitions the state on-disk and in-memory to kError.
    void _transitionStateToError(const Status& status);

    // Updates the recipient document on-disk and in-memory with the 'replacementDoc.'
    void _updateRecipientDocument(ReshardingRecipientDocument&& replacementDoc);

    // The in-memory representation of the underlying document in
    // config.localReshardingOperations.recipient.
    ReshardingRecipientDocument _recipientDoc;

    // Each promise below corresponds to a state on the recipient state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled.
    SharedPromise<Timestamp> _allDonorsPreparedToDonate;

    SharedPromise<void> _allDonorsMirroring;

    SharedPromise<void> _coordinatorHasCommitted;

    // The id both for the resharding operation and for the primary-only-service instance.
    const UUID _id;
};

}  // namespace mongo

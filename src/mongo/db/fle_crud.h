/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
class OperationContext;

/**
 * FLE Result enum
 */
enum class FLEBatchResult {
    /**
     * FLE CRUD code decided input document requires FLE processing. Caller should not do any CRUD.
     */
    kProcessed,

    /**
     * FLE CRUD code decided it did not have to do any CRUD. For instance, it has no encrypted
     * fields that require further processing. Caller should process the request normally.
     */
    kNotProcessed
};

/**
 * Process a batch from mongos.
 */
FLEBatchResult processFLEBatch(OperationContext* opCtx,
                               const BatchedCommandRequest& request,
                               BatchWriteExecStats* stats,
                               BatchedCommandResponse* response,
                               boost::optional<OID> targetEpoch);

/**
 * Rewrite a BatchedCommandRequest for explain commands.
 */
std::unique_ptr<BatchedCommandRequest> processFLEBatchExplain(OperationContext* opCtx,
                                                              const BatchedCommandRequest& request);


/**
 * Initialize the FLE CRUD subsystem on Mongod.
 */
void startFLECrud(ServiceContext* serviceContext);

/**
 * Stop the FLE CRUD subsystem on Mongod.
 */
void stopFLECrud();


/**
 * Process a replica set insert.
 */
FLEBatchResult processFLEInsert(OperationContext* opCtx,
                                const write_ops::InsertCommandRequest& insertRequest,
                                write_ops::InsertCommandReply* insertReply);

/**
 * Process a replica set delete.
 */
write_ops::DeleteCommandReply processFLEDelete(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& deleteRequest);

/**
 * Rewrite the query within a replica set explain command for delete and update.
 * This concrete function is passed all the parameters directly.
 */
BSONObj processFLEWriteExplainD(OperationContext* opCtx,
                                const BSONObj& collation,
                                const NamespaceString& nss,
                                const EncryptionInformation& info,
                                const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
                                const boost::optional<BSONObj>& letParameters,
                                const BSONObj& query);

/**
 * Rewrite the query within a replica set explain command for delete and update.
 * This template is passed the request object from the command and delegates
 * to the function above.
 */
template <typename T>
BSONObj processFLEWriteExplainD(OperationContext* opCtx,
                                const BSONObj& collation,
                                const T& request,
                                const BSONObj& query) {

    return processFLEWriteExplainD(opCtx,
                                   collation,
                                   request.getNamespace(),
                                   request.getEncryptionInformation().get(),
                                   request.getLegacyRuntimeConstants(),
                                   request.getLet(),
                                   query);
}

/**
 * Process a replica set update.
 */
write_ops::UpdateCommandReply processFLEUpdate(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& updateRequest);

/**
 * Process a findAndModify request from mongos
 */
FLEBatchResult processFLEFindAndModify(OperationContext* opCtx,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder& result);

std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>
processFLEFindAndModifyExplainMongos(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

/**
 * Process a findAndModify request from a replica set.
 */
write_ops::FindAndModifyCommandReply processFLEFindAndModify(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>
processFLEFindAndModifyExplainMongod(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

/**
 * Process a find command from mongos.
 */
void processFLEFindS(OperationContext* opCtx,
                     const NamespaceString& nss,
                     FindCommandRequest* findCommand);

/**
 * Process a find command from a replica set.
 */
void processFLEFindD(OperationContext* opCtx,
                     const NamespaceString& nss,
                     FindCommandRequest* findCommand);


/**
 * Process a find command from mongos.
 */
void processFLECountS(OperationContext* opCtx,
                      const NamespaceString& nss,
                      CountCommandRequest* countCommand);

/**
 * Process a find command from a replica set.
 */
void processFLECountD(OperationContext* opCtx,
                      const NamespaceString& nss,
                      CountCommandRequest* countCommand);

/**
 * Process a pipeline from mongos.
 */
std::unique_ptr<Pipeline, PipelineDeleter> processFLEPipelineS(
    OperationContext* opCtx,
    NamespaceString nss,
    const EncryptionInformation& encryptInfo,
    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite);

/**
 * Process a pipeline from a replica set.
 */
std::unique_ptr<Pipeline, PipelineDeleter> processFLEPipelineD(
    OperationContext* opCtx,
    NamespaceString nss,
    const EncryptionInformation& encryptInfo,
    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite);

/**
 * Helper function to determine if an IDL object with encryption information should be rewritten.
 */
template <typename T>
bool shouldDoFLERewrite(const std::unique_ptr<T>& cmd) {
    return cmd->getEncryptionInformation().has_value();
}

template <typename T>
bool shouldDoFLERewrite(const T& cmd) {
    return cmd.getEncryptionInformation().has_value();
}

/**
 * Abstraction layer for FLE
 */
class FLEQueryInterface : public FLETagQueryInterface {
public:
    /**
     * Insert a document into the given collection.
     *
     * If translateDuplicateKey == true and the insert returns DuplicateKey, returns
     * FLEStateCollectionContention instead.
     */
    virtual StatusWith<write_ops::InsertCommandReply> insertDocuments(
        const NamespaceString& nss,
        std::vector<BSONObj> objs,
        StmtId* pStmtId,
        bool translateDuplicateKey,
        bool bypassDocumentValidation = false) = 0;

    /**
     * Delete a single document with the given query.
     *
     * Returns the pre-image of the deleted document. If no documents were deleted, returns an empty
     * BSON object.
     */
    virtual std::pair<write_ops::DeleteCommandReply, BSONObj> deleteWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::DeleteCommandRequest& deleteRequest) = 0;

    virtual write_ops::DeleteCommandReply deleteDocument(
        const NamespaceString& nss,
        int32_t stmtId,
        write_ops::DeleteCommandRequest& deleteRequest) = 0;

    /**
     * Update a single document with the given query and update operators.
     *
     * Returns the pre-image of the updated document. If no documents were updated, returns an empty
     * BSON object.
     */
    virtual std::pair<write_ops::UpdateCommandReply, BSONObj> updateWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::UpdateCommandRequest& updateRequest) = 0;


    /**
     * Update a single document with the given query and update operators.
     *
     * Returns an update reply.
     */
    virtual write_ops::UpdateCommandReply update(
        const NamespaceString& nss,
        int32_t stmtId,
        write_ops::UpdateCommandRequest& updateRequest) = 0;

    /**
     * Do a single findAndModify request.
     *
     * Returns a findAndModify reply.
     */
    virtual write_ops::FindAndModifyCommandReply findAndModify(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) = 0;

    /**
     * Find a document with the given filter.
     */
    virtual std::vector<BSONObj> findDocuments(const NamespaceString& nss, BSONObj filter) = 0;
};

/**
 * Implementation of the FLE Query interface that exposes the DB operations needed for FLE 2
 * server-side work.
 */
class FLEQueryInterfaceImpl : public FLEQueryInterface {
public:
    FLEQueryInterfaceImpl(const txn_api::TransactionClient& txnClient,
                          ServiceContext* serviceContext)
        : _txnClient(txnClient), _serviceContext(serviceContext) {}

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    uint64_t countDocuments(const NamespaceString& nss) final;

    std::vector<std::vector<FLEEdgeCountInfo>> getTags(
        const NamespaceString& nss,
        const std::vector<std::vector<FLEEdgePrfBlock>>& escDerivedFromDataTokenAndCounter,
        FLETagQueryInterface::TagQueryType type) final;

    StatusWith<write_ops::InsertCommandReply> insertDocuments(
        const NamespaceString& nss,
        std::vector<BSONObj> objs,
        StmtId* pStmtId,
        bool translateDuplicateKey,
        bool bypassDocumentValidation = false) final;

    std::pair<write_ops::DeleteCommandReply, BSONObj> deleteWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::DeleteCommandRequest& deleteRequest) final;

    write_ops::DeleteCommandReply deleteDocument(
        const NamespaceString& nss,
        int32_t stmtId,
        write_ops::DeleteCommandRequest& deleteRequest) final;

    std::pair<write_ops::UpdateCommandReply, BSONObj> updateWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::UpdateCommandRequest& updateRequest) final;

    write_ops::UpdateCommandReply update(const NamespaceString& nss,
                                         int32_t stmtId,
                                         write_ops::UpdateCommandRequest& updateRequest) final;

    write_ops::FindAndModifyCommandReply findAndModify(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) final;

    std::vector<BSONObj> findDocuments(const NamespaceString& nss, BSONObj filter) final;

private:
    const txn_api::TransactionClient& _txnClient;
    ServiceContext* _serviceContext;
    std::shared_ptr<executor::InlineExecutor::SleepableExecutor> _executor;
};

/**
 * FLETagQueryInterface that does not use transaction_api.h to retrieve tags.
 */
class FLETagNoTXNQuery : public FLETagQueryInterface {
public:
    FLETagNoTXNQuery(OperationContext* opCtx);

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    uint64_t countDocuments(const NamespaceString& nss) final;

    std::vector<std::vector<FLEEdgeCountInfo>> getTags(
        const NamespaceString& nss,
        const std::vector<std::vector<FLEEdgePrfBlock>>& escDerivedFromDataTokenAndCounter,
        FLETagQueryInterface::TagQueryType type) final;

private:
    OperationContext* _opCtx;
};

/**
 * Creates a new SyncTransactionWithRetries object that runs a transaction on the
 * sharding fixed task executor.
 */
std::shared_ptr<txn_api::SyncTransactionWithRetries> getTransactionWithRetriesForMongoS(
    OperationContext* opCtx);

/**
 * Creates a new SyncTransactionWithRetries object that runs a transaction on a
 * thread pool local to mongod.
 */
std::shared_ptr<txn_api::SyncTransactionWithRetries> getTransactionWithRetriesForMongoD(
    OperationContext* opCtx);

/**
 * Process a FLE insert with the query interface
 *
 * Used by unit tests.
 */
StatusWith<write_ops::InsertCommandReply> processInsert(
    FLEQueryInterface* queryImpl,
    const NamespaceString& edcNss,
    std::vector<EDCServerPayloadInfo>& serverPayload,
    const EncryptedFieldConfig& efc,
    int32_t* stmtId,
    BSONObj document,
    bool bypassDocumentValidation = false);

/**
 * Process a FLE delete with the query interface
 *
 * Used by unit tests.
 */
write_ops::DeleteCommandReply processDelete(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest);

/**
 * Process a FLE Update with the query interface
 *
 * Used by unit tests.
 */
write_ops::UpdateCommandReply processUpdate(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest);

/**
 * Process a FLE Find And Modify with the query interface
 *
 * Used by unit tests.
 */
write_ops::FindAndModifyCommandReply processFindAndModify(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

write_ops::FindAndModifyCommandRequest processFindAndModifyExplain(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

/**
 * Callback function to get a SyncTransactionWithRetries with the appropiate Executor
 */
using GetTxnCallback =
    std::function<std::shared_ptr<txn_api::SyncTransactionWithRetries>(OperationContext*)>;

std::pair<FLEBatchResult, write_ops::InsertCommandReply> processInsert(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& insertRequest,
    GetTxnCallback getTxns);

write_ops::DeleteCommandReply processDelete(OperationContext* opCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest,
                                            GetTxnCallback getTxns);

template <typename ReplyType>
using ProcessFindAndModifyCallback =
    std::function<ReplyType(boost::intrusive_ptr<ExpressionContext> expCtx,
                            FLEQueryInterface* queryImpl,
                            const write_ops::FindAndModifyCommandRequest& findAndModifyRequest)>;

template <typename ReplyType>
StatusWith<std::pair<ReplyType, OpMsgRequest>> processFindAndModifyRequest(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<ReplyType> processCallback = processFindAndModify);

extern template StatusWith<std::pair<write_ops::FindAndModifyCommandReply, OpMsgRequest>>
processFindAndModifyRequest<write_ops::FindAndModifyCommandReply>(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<write_ops::FindAndModifyCommandReply> processCallback);

extern template StatusWith<std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>>
processFindAndModifyRequest<write_ops::FindAndModifyCommandRequest>(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<write_ops::FindAndModifyCommandRequest> processCallback);

write_ops::UpdateCommandReply processUpdate(OperationContext* opCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest,
                                            GetTxnCallback getTxns);

void validateInsertUpdatePayloads(const std::vector<EncryptedField>& fields,
                                  const std::vector<EDCServerPayloadInfo>& payload);

/**
 * Get the tags from local storage.
 */
std::vector<std::vector<FLEEdgeCountInfo>> getTagsFromStorage(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const std::vector<std::vector<FLEEdgePrfBlock>>& escDerivedFromDataTokens,
    FLETagQueryInterface::TagQueryType type);

}  // namespace mongo

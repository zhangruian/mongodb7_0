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

#include "mongo/platform/basic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "mongo/db/fle_crud.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace {

class FLEQueryInterfaceImpl : public FLEQueryInterface {
public:
    FLEQueryInterfaceImpl(const txn_api::TransactionClient& txnClient) : _txnClient(txnClient) {}

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    uint64_t countDocuments(const NamespaceString& nss) final;

    void insertDocument(const NamespaceString& nss, BSONObj obj, bool translateDuplicateKey) final;

    BSONObj deleteWithPreimage(const NamespaceString& nss,
                               const EncryptionInformation& ei,
                               const write_ops::DeleteCommandRequest& deleteRequest) final;

    BSONObj updateWithPreimage(const NamespaceString& nss,
                               const EncryptionInformation& ei,
                               const write_ops::UpdateCommandRequest& updateRequest) final;

private:
    const txn_api::TransactionClient& _txnClient;
};

BSONObj FLEQueryInterfaceImpl::getById(const NamespaceString& nss, BSONElement element) {
    FindCommandRequest find(nss);
    find.setFilter(BSON("_id" << element));
    find.setSingleBatch(true);

    // Throws on error
    auto docs = _txnClient.exhaustiveFind(find).get();

    if (docs.size() == 0) {
        return BSONObj();
    } else {
        // We only expect one document in the state collection considering that _id is a unique
        // index
        uassert(6371201,
                "Unexpected to find more then one FLE state collection document",
                docs.size() == 1);
        return docs[0];
    }
}

uint64_t FLEQueryInterfaceImpl::countDocuments(const NamespaceString& nss) {
    // TODO - what about
    // count cmd
    // $collStats
    // approxCount

    // Build the following pipeline:
    //
    //{ aggregate : "testColl", pipeline: [{$match:{}}, {$group : {_id: null, n : {$sum:1}
    //}} ], cursor: {}}

    BSONObjBuilder builder;
    // $db - TXN API DOES THIS FOR US by building OP_MSG
    builder.append("aggregate", nss.coll());

    AggregateCommandRequest request(nss);

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSONObj()));

    {
        BSONObjBuilder sub;
        {
            BSONObjBuilder sub2(sub.subobjStart("$group"));
            sub2.appendNull("_id");
            {
                BSONObjBuilder sub3(sub.subobjStart("n"));
                sub3.append("$sum", 1);
            }
        }

        pipeline.push_back(sub.obj());
    }

    request.setPipeline(pipeline);

    auto commandResponse = _txnClient.runCommand(nss.db(), request.toBSON({})).get();

    uint64_t docCount = 0;
    auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(commandResponse));

    auto firstBatch = cursorResponse.getBatch();
    if (!firstBatch.empty()) {
        auto countObj = firstBatch.front();
        docCount = countObj.getIntField("n"_sd);
    }

    return docCount;
}

void FLEQueryInterfaceImpl::insertDocument(const NamespaceString& nss,
                                           BSONObj obj,
                                           bool translateDuplicateKey) {
    write_ops::InsertCommandRequest insertRequest(nss);
    insertRequest.setDocuments({obj});
    // TODO SERVER-64143 - insertRequest.setWriteConcern

    // TODO SERVER-64143 - propagate the retryable statement ids to runCRUDOp
    auto response = _txnClient.runCRUDOp(BatchedCommandRequest(insertRequest), {}).get();

    auto status = response.toStatus();
    if (translateDuplicateKey && status.code() == ErrorCodes::DuplicateKey) {
        uassertStatusOK(Status(ErrorCodes::FLEStateCollectionContention, status.reason()));
    }

    uassertStatusOK(status);
}

BSONObj FLEQueryInterfaceImpl::deleteWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::DeleteCommandRequest& deleteRequest) {
    auto deleteOpEntry = deleteRequest.getDeletes()[0];

    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss);
    findAndModifyRequest.setQuery(deleteOpEntry.getQ());
    findAndModifyRequest.setHint(deleteOpEntry.getHint());
    findAndModifyRequest.setBatchSize(1);
    findAndModifyRequest.setSingleBatch(true);
    findAndModifyRequest.setRemove(true);
    findAndModifyRequest.setCollation(deleteOpEntry.getCollation());
    findAndModifyRequest.setLet(deleteRequest.getLet());
    // TODO SERVER-64143 - writeConcern
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    // TODO SERVER-63714 - findAndModifyRequest.setEncryptedTokens(ei2);

    auto response = _txnClient.runCommand(nss.db(), findAndModifyRequest.toBSON({})).get();
    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply =
        write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("reply"), response);

    if (!reply.getValue().has_value()) {
        return BSONObj();
    }

    return reply.getValue().value();
}

boost::optional<BSONObj> mergeLetAndCVariables(const boost::optional<BSONObj>& let,
                                               const boost::optional<BSONObj>& c) {
    if (!let.has_value() && !c.has_value()) {
        return boost::none;
    } else if (let.has_value() && c.has_value()) {
        BSONObj obj = let.value();
        // Prioritize the fields in c over the fields in let in case of duplicates
        obj.addFields(c.value());
        return {obj};
    } else if (let.has_value()) {
        return let;
    }
    return c;
}

BSONObj FLEQueryInterfaceImpl::updateWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::UpdateCommandRequest& updateRequest) {
    auto updateOpEntry = updateRequest.getUpdates()[0];

    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss);
    findAndModifyRequest.setQuery(updateOpEntry.getQ());
    findAndModifyRequest.setUpdate(updateOpEntry.getU());
    findAndModifyRequest.setBatchSize(1);
    findAndModifyRequest.setUpsert(false);
    findAndModifyRequest.setSingleBatch(true);
    findAndModifyRequest.setRemove(false);
    findAndModifyRequest.setArrayFilters(updateOpEntry.getArrayFilters());
    findAndModifyRequest.setCollation(updateOpEntry.getCollation());
    findAndModifyRequest.setHint(updateOpEntry.getHint());
    findAndModifyRequest.setLet(
        mergeLetAndCVariables(updateRequest.getLet(), updateOpEntry.getC()));
    // TODO SERVER-64143 - writeConcern
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    // TODO SERVER-63714 - findAndModifyRequest.setEncryptedTokens(ei2);

    auto response = _txnClient.runCommand(nss.db(), findAndModifyRequest.toBSON({})).get();
    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply =
        write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("reply"), response);

    if (!reply.getValue().has_value()) {
        return BSONObj();
    }

    return reply.getValue().value();
}

/**
 * Implementation of FLEStateCollectionReader for txn_api::TransactionClient
 *
 * Document count is cached since we only need it once per esc or ecc collection.
 */
class TxnCollectionReader : public FLEStateCollectionReader {
public:
    TxnCollectionReader(uint64_t count, FLEQueryInterface* queryImpl, const NamespaceString& nss)
        : _count(count), _queryImpl(queryImpl), _nss(nss) {}

    uint64_t getDocumentCount() override {
        return _count;
    }

    BSONObj getById(PrfBlock block) override {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        BSONElement element = doc.firstElement();
        return _queryImpl->getById(_nss, element);
    }

private:
    uint64_t _count;
    FLEQueryInterface* _queryImpl;
    const NamespaceString& _nss;
};

StatusWith<txn_api::CommitResult> runInTxnWithRetry(
    OperationContext* opCtx,
    std::shared_ptr<txn_api::TransactionWithRetries> trun,
    std::function<SemiFuture<void>(const txn_api::TransactionClient& txnClient,
                                   ExecutorPtr txnExec)> callback) {

    bool inClientTransaction = opCtx->inMultiDocumentTransaction();

    // TODO SERVER-59566 - how much do we retry before we give up?
    while (true) {

        // Result will get the status of the TXN
        // Non-client initiated txns get retried automatically.
        // Client txns are the user responsibility to retry and so if we hit a contention
        // placeholder, we need to abort and defer to the client
        auto swResult = trun->runSyncNoThrow(opCtx, callback);
        if (swResult.isOK()) {
            return swResult;
        }

        // We cannot retry the transaction if initiated by a user
        if (inClientTransaction) {
            return swResult;
        }

        // - DuplicateKeyException - suggestions contention on ESC
        // - FLEContention
        if (swResult.getStatus().code() != ErrorCodes::FLECompactionPlaceholder &&
            swResult.getStatus().code() != ErrorCodes::FLEStateCollectionContention) {
            return swResult;
        }

        if (!swResult.isOK()) {
            return swResult;
        }

        auto commitResult = swResult.getValue();
        if (commitResult.getEffectiveStatus().isOK()) {
            return commitResult;
        }
    }
}

StatusWith<FLEBatchResult> processInsert(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& insertRequest,
    std::function<std::shared_ptr<txn_api::TransactionWithRetries>(OperationContext*)> getTxns) {

    auto documents = insertRequest.getDocuments();
    // TODO - how to check if a document will be too large???
    uassert(6371202, "Only single insert batches are supported in FLE2", documents.size() == 1);

    auto document = documents[0];
    auto serverPayload = std::make_shared<std::vector<EDCServerPayloadInfo>>(
        EDCServerCollection::getEncryptedFieldInfo(document));

    if (serverPayload->size() == 0) {
        // No actual FLE2 indexed fields
        return FLEBatchResult::kNotProcessed;
    }

    auto ei = insertRequest.getEncryptionInformation().get();

    auto edcNss = insertRequest.getNamespace();
    auto efc = EncryptionInformationHelpers::getAndValidateSchema(insertRequest.getNamespace(), ei);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs since it runs on another thread
    auto ownedDocument = document.getOwned();
    auto insertBlock = std::tie(edcNss, efc, serverPayload);
    auto sharedInsertBlock = std::make_shared<decltype(insertBlock)>(insertBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedInsertBlock, ownedDocument](const txn_api::TransactionClient& txnClient,
                                           ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [edcNss2, efc2, serverPayload2] = *sharedInsertBlock.get();

            processInsert(&queryImpl, edcNss2, *serverPayload2.get(), efc2, ownedDocument);

            return SemiFuture<void>::makeReady();
        });
    if (!swResult.isOK()) {
        return swResult.getStatus();
    }

    return FLEBatchResult::kProcessed;
}

StatusWith<std::pair<FLEBatchResult, uint64_t>> processDelete(
    OperationContext* opCtx,
    const write_ops::DeleteCommandRequest& deleteRequest,
    std::function<std::shared_ptr<txn_api::TransactionWithRetries>(OperationContext*)> getTxns) {

    auto deletes = deleteRequest.getDeletes();
    uassert(6371302, "Only single document deletes are permitted", deletes.size() == 1);

    auto deleteOpEntry = deletes[0];

    uassert(
        6371303, "FLE only supports single document deletes", deleteOpEntry.getMulti() == false);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    uint64_t count = 0;
    auto deleteBlock = std::tie(deleteRequest, count);
    auto sharedDeleteBlock = std::make_shared<decltype(deleteBlock)>(deleteBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedDeleteBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [deleteRequest2, count] = *sharedDeleteBlock.get();

            count = processDelete(&queryImpl, deleteRequest2);

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        return swResult.getStatus();
    }

    return std::pair{FLEBatchResult::kProcessed, count};
}

StatusWith<std::pair<FLEBatchResult, uint64_t>> processUpdate(
    OperationContext* opCtx,
    const write_ops::UpdateCommandRequest& updateRequest,
    std::function<std::shared_ptr<txn_api::TransactionWithRetries>(OperationContext*)> getTxns) {

    auto updates = updateRequest.getUpdates();
    uassert(6371502, "Only single document updates are permitted", updates.size() == 1);

    auto updateOpEntry = updates[0];

    uassert(
        6371503, "FLE only supports single document updates", updateOpEntry.getMulti() == false);

    // pipeline - is agg specific, delta is oplog, transform is internal (timeseries)
    uassert(6371517,
            "FLE only supports modifier and replacement style updates",
            updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kModifier ||
                updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kReplacement);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    uint64_t count = 0;
    auto updateBlock = std::tie(updateRequest, count);
    auto sharedupdateBlock = std::make_shared<decltype(updateBlock)>(updateBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedupdateBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [updateRequest2, count] = *sharedupdateBlock.get();

            count = processUpdate(&queryImpl, updateRequest2);

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        return swResult.getStatus();
    }

    return std::pair{FLEBatchResult::kProcessed, count};
}

void processFieldsForInsert(FLEQueryInterface* queryImpl,
                            const NamespaceString& edcNss,
                            std::vector<EDCServerPayloadInfo>& serverPayload,
                            const EncryptedFieldConfig& efc) {

    NamespaceString nssEsc(edcNss.db(), efc.getEscCollection().get());

    auto docCount = queryImpl->countDocuments(nssEsc);

    TxnCollectionReader reader(docCount, queryImpl, nssEsc);

    for (auto& payload : serverPayload) {

        auto escToken = payload.getESCToken();
        auto tagToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escToken);
        auto valueToken =
            FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escToken);

        int position = 1;
        int count = 1;
        auto alpha = ESCCollection::emuBinary(&reader, tagToken, valueToken);

        if (alpha.has_value() && alpha.value() == 0) {
            position = 1;
            count = 1;
        } else if (!alpha.has_value()) {
            auto block = ESCCollection::generateId(tagToken, boost::none);

            auto r_esc = reader.getById(block);
            uassert(6371203, "ESC document not found", !r_esc.isEmpty());

            auto escNullDoc =
                uassertStatusOK(ESCCollection::decryptNullDocument(valueToken, r_esc));

            position = escNullDoc.position + 2;
            count = escNullDoc.count + 1;
        } else {
            auto block = ESCCollection::generateId(tagToken, alpha);

            auto r_esc = reader.getById(block);
            uassert(6371204, "ESC document not found", !r_esc.isEmpty());

            auto escDoc = uassertStatusOK(ESCCollection::decryptDocument(valueToken, r_esc));

            position = alpha.value() + 1;
            count = escDoc.count + 1;

            if (escDoc.compactionPlaceholder) {
                uassertStatusOK(Status(ErrorCodes::FLECompactionPlaceholder,
                                       "Found ESC contention placeholder"));
            }
        }

        payload.count = count;

        queryImpl->insertDocument(
            nssEsc,
            ESCCollection::generateInsertDocument(tagToken, valueToken, position, count),
            true);

        NamespaceString nssEcoc(edcNss.db(), efc.getEcocCollection().get());

        // TODO - should we make this a batch of ECOC updates?
        queryImpl->insertDocument(nssEcoc,
                                  ECOCCollection::generateDocument(
                                      payload.fieldPathName, payload.payload.getEncryptedTokens()),
                                  false);
    }
}

void processRemovedFields(FLEQueryInterface* queryImpl,
                          const NamespaceString& edcNss,
                          const EncryptedFieldConfig& efc,
                          const StringMap<FLEDeleteToken>& tokenMap,
                          const std::vector<EDCIndexedFields>& deletedFields) {

    NamespaceString nssEcc(edcNss.db(), efc.getEccCollection().get());


    auto docCount = queryImpl->countDocuments(nssEcc);

    TxnCollectionReader reader(docCount, queryImpl, nssEcc);


    for (const auto& deletedField : deletedFields) {
        // TODO - verify each indexed fields is listed in EncryptionInformation for the
        // schema

        auto it = tokenMap.find(deletedField.fieldPathName);
        uassert(6371304,
                str::stream() << "Could not find delete token for field: "
                              << deletedField.fieldPathName,
                it != tokenMap.end());

        auto deleteToken = it->second;

        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(deletedField.value);

        // TODO - add support other types
        uassert(6371305,
                "Ony support deleting equality indexed fields",
                encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValue);

        auto plainTextField = uassertStatusOK(FLE2IndexedEqualityEncryptedValue::decryptAndParse(
            deleteToken.serverEncryptionToken, subCdr));

        auto tagToken =
            FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(plainTextField.ecc);
        auto valueToken =
            FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(plainTextField.ecc);

        auto alpha = ECCCollection::emuBinary(&reader, tagToken, valueToken);

        uint64_t index = 0;
        if (alpha.has_value() && alpha.value() == 0) {
            index = 1;
        } else if (!alpha.has_value()) {
            auto block = ECCCollection::generateId(tagToken, boost::none);

            auto r_ecc = reader.getById(block);
            uassert(6371306, "ECC null document not found", !r_ecc.isEmpty());

            auto eccNullDoc =
                uassertStatusOK(ECCCollection::decryptNullDocument(valueToken, r_ecc));
            index = eccNullDoc.position + 2;
        } else {
            auto block = ECCCollection::generateId(tagToken, alpha);

            auto r_ecc = reader.getById(block);
            uassert(6371307, "ECC document not found", !r_ecc.isEmpty());

            auto eccDoc = uassertStatusOK(ECCCollection::decryptDocument(valueToken, r_ecc));

            if (eccDoc.valueType == ECCValueType::kCompactionPlaceholder) {
                uassertStatusOK(
                    Status(ErrorCodes::FLECompactionPlaceholder, "Found contention placeholder"));
            }

            index = alpha.value() + 1;
        }

        queryImpl->insertDocument(
            nssEcc,
            ECCCollection::generateDocument(tagToken, valueToken, index, plainTextField.count),
            true);

        NamespaceString nssEcoc(edcNss.db(), efc.getEcocCollection().get());

        // TODO - make this a batch of ECOC updates?
        EncryptedStateCollectionTokens tokens(plainTextField.esc, plainTextField.ecc);
        auto encryptedTokens = uassertStatusOK(tokens.serialize(deleteToken.ecocToken));
        queryImpl->insertDocument(
            nssEcoc,
            ECOCCollection::generateDocument(deletedField.fieldPathName, encryptedTokens),
            false);
    }
}

}  // namespace

FLEQueryInterface::~FLEQueryInterface() {}

void processInsert(FLEQueryInterface* queryImpl,
                   const NamespaceString& edcNss,
                   std::vector<EDCServerPayloadInfo>& serverPayload,
                   const EncryptedFieldConfig& efc,
                   BSONObj document) {

    processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

    auto finalDoc = EDCServerCollection::finalizeForInsert(document, serverPayload);

    queryImpl->insertDocument(edcNss, finalDoc, false);
}

uint64_t processDelete(FLEQueryInterface* queryImpl,
                       const write_ops::DeleteCommandRequest& deleteRequest) {
    auto edcNss = deleteRequest.getNamespace();
    auto ei = deleteRequest.getEncryptionInformation().get();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);
    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(edcNss, ei);

    // TODO SERVER-64143 - use this delete for retryable writes
    BSONObj deletedDocument = queryImpl->deleteWithPreimage(edcNss, ei, deleteRequest);

    // If the delete did not actually delete anything, we are done
    if (deletedDocument.isEmpty()) {
        return 0;
    }

    auto deletedFields = EDCServerCollection::getEncryptedIndexedFields(deletedDocument);

    processRemovedFields(queryImpl, edcNss, efc, tokenMap, deletedFields);

    return 1;
}

/**
 * Update is the most complicated FLE operation.
 * It is basically an insert followed by a delete, sort of.
 *
 * 1. Process the update for any encrypted fields like insert, update the ESC and get new counters
 * 2. Extend the update $push new tags into the document
 * 3. Run the update with findAndModify to get the pre-image
 * 4. Run a find to get the post-image update with the id from the pre-image
 * -- Fail if we cannot find the new document. This could happen if they updated _id.
 * 5. Find the removed fields and update ECC
 * 6. Remove the stale tags from the original document with a new push
 */
uint64_t processUpdate(FLEQueryInterface* queryImpl,
                       const write_ops::UpdateCommandRequest& updateRequest) {

    auto edcNss = updateRequest.getNamespace();
    auto ei = updateRequest.getEncryptionInformation().get();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);
    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(edcNss, ei);

    auto updateOpEntry = updateRequest.getUpdates()[0];
    auto updateModifier = updateOpEntry.getU().getUpdateModifier();

    // Step 1 ----
    auto serverPayload = std::vector<EDCServerPayloadInfo>(
        EDCServerCollection::getEncryptedFieldInfo(updateModifier));

    processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

    // Step 2 ----
    auto pushUpdate = EDCServerCollection::finalizeForUpdate(updateModifier, serverPayload);

    // Step 3 ----
    auto newUpdateOpEntry = updateRequest.getUpdates()[0];

    newUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
        pushUpdate, write_ops::UpdateModification::ClassicTag(), false));
    auto newUpdateRequest = updateRequest;
    newUpdateRequest.setUpdates({newUpdateOpEntry});

    // TODO - use this update for retryable writes
    BSONObj originalDocument = queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);
    if (originalDocument.isEmpty()) {
        // if there is no preimage, then we did not update any documents, we are done
        return 0;
    }

    // Step 4 ----
    auto idElement = originalDocument.firstElement();
    uassert(6371504,
            "Missing _id field in pre-image document",
            idElement.fieldNameStringData() == "_id"_sd);
    BSONObj newDocument = queryImpl->getById(edcNss, idElement);

    // Fail if we could not find the new document
    uassert(6371505, "Could not find pre-image document by _id", !newDocument.isEmpty());

    // Check the user did not remove/destroy the __safeContent__ array
    FLEClientCrypto::validateTagsArray(newDocument);

    // Step 5 ----
    auto originalFields = EDCServerCollection::getEncryptedIndexedFields(originalDocument);
    auto newFields = EDCServerCollection::getEncryptedIndexedFields(newDocument);
    auto deletedFields = EDCServerCollection::getRemovedTags(originalFields, newFields);

    processRemovedFields(queryImpl, edcNss, efc, tokenMap, deletedFields);

    // Step 6 ----
    BSONObj pullUpdate = EDCServerCollection::generateUpdateToRemoveTags(deletedFields, tokenMap);
    auto pullUpdateOpEntry = write_ops::UpdateOpEntry();
    pullUpdateOpEntry.setUpsert(false);
    pullUpdateOpEntry.setMulti(false);
    pullUpdateOpEntry.setQ(BSON("_id"_sd << idElement));
    pullUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
        pullUpdate, write_ops::UpdateModification::ClassicTag(), false));
    newUpdateRequest.setUpdates({pullUpdateOpEntry});
    BSONObj finalCorrectDocument = queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);

    return 1;
}


FLEBatchResult processFLEBatch(OperationContext* opCtx,
                               const BatchedCommandRequest& request,
                               BatchWriteExecStats* stats,
                               BatchedCommandResponse* response,
                               boost::optional<OID> targetEpoch) {

    if (!gFeatureFlagFLE2.isEnabledAndIgnoreFCV()) {
        uasserted(6371209, "Feature flag FLE2 is not enabled");
    }

    auto getTxn = [](OperationContext* opCtx) {
        return std::make_shared<txn_api::TransactionWithRetries>(
            opCtx,
            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
            TransactionRouterResourceYielder::make());
    };

    if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert) {
        auto insertRequest = request.getInsertRequest();

        auto swResult = processInsert(opCtx, insertRequest, getTxn);

        if (!swResult.isOK()) {
            response->setStatus(swResult.getStatus());
            response->setN(0);

            return FLEBatchResult::kProcessed;
        } else if (swResult.getValue() == FLEBatchResult::kProcessed) {
            response->setStatus(Status::OK());
            response->setN(1);
        }

        return swResult.getValue();
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {

        auto deleteRequest = request.getDeleteRequest();

        auto swResult = processDelete(opCtx, deleteRequest, getTxn);


        if (!swResult.isOK()) {
            response->setStatus(swResult.getStatus());
            response->setN(0);

            return FLEBatchResult::kProcessed;
        } else if (swResult.getValue().first == FLEBatchResult::kProcessed) {
            response->setStatus(Status::OK());
            response->setN(swResult.getValue().second);
        }

        return swResult.getValue().first;
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {

        auto updateRequest = request.getUpdateRequest();

        auto swResult = processUpdate(opCtx, updateRequest, getTxn);

        if (!swResult.isOK()) {
            response->setStatus(swResult.getStatus());
            response->setN(0);
            response->setNModified(0);

            return FLEBatchResult::kProcessed;
        } else if (swResult.getValue().first == FLEBatchResult::kProcessed) {
            response->setStatus(Status::OK());
            response->setN(swResult.getValue().second);
            response->setNModified(swResult.getValue().second);
        }

        return swResult.getValue().first;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo

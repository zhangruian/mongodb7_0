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

#pragma once

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/ops/write_ops.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/util/visit_helper.h"

namespace mongo {

/**
 * This class wraps the different kinds of command requests into a generically usable write command
 * request that can be passed around.
 */
class BatchedCommandRequest {
public:
    enum BatchType { BatchType_Insert, BatchType_Update, BatchType_Delete };

    BatchedCommandRequest(write_ops::Insert insertOp)
        : _batchType(BatchType_Insert),
          _insertReq(std::make_unique<write_ops::Insert>(std::move(insertOp))) {}

    BatchedCommandRequest(write_ops::Update updateOp)
        : _batchType(BatchType_Update),
          _updateReq(std::make_unique<write_ops::Update>(std::move(updateOp))) {}

    BatchedCommandRequest(write_ops::Delete deleteOp)
        : _batchType(BatchType_Delete),
          _deleteReq(std::make_unique<write_ops::Delete>(std::move(deleteOp))) {}

    BatchedCommandRequest(BatchedCommandRequest&&) = default;

    static BatchedCommandRequest parseInsert(const OpMsgRequest& request);
    static BatchedCommandRequest parseUpdate(const OpMsgRequest& request);
    static BatchedCommandRequest parseDelete(const OpMsgRequest& request);

    BatchType getBatchType() const {
        return _batchType;
    }

    const NamespaceString& getNS() const;

    const auto& getInsertRequest() const {
        invariant(_insertReq);
        return *_insertReq;
    }

    const auto& getUpdateRequest() const {
        invariant(_updateReq);
        return *_updateReq;
    }

    const auto& getDeleteRequest() const {
        invariant(_deleteReq);
        return *_deleteReq;
    }

    std::size_t sizeWriteOps() const;

    void setWriteConcern(const BSONObj& writeConcern) {
        _writeConcern = writeConcern.getOwned();
    }

    void unsetWriteConcern() {
        _writeConcern = boost::none;
    }

    bool hasWriteConcern() const {
        return _writeConcern.is_initialized();
    }

    const BSONObj& getWriteConcern() const {
        invariant(_writeConcern);
        return *_writeConcern;
    }

    bool isVerboseWC() const;

    void setShardVersion(ChunkVersion shardVersion) {
        _shardVersion = std::move(shardVersion);
    }

    bool hasShardVersion() const {
        return _shardVersion.is_initialized();
    }

    const ChunkVersion& getShardVersion() const {
        invariant(_shardVersion);
        return *_shardVersion;
    }

    void setDbVersion(DatabaseVersion dbVersion) {
        _dbVersion = std::move(dbVersion);
    }

    bool hasDbVersion() const {
        return _dbVersion.is_initialized();
    }

    const DatabaseVersion& getDbVersion() const {
        invariant(_dbVersion);
        return *_dbVersion;
    }

    void setRuntimeConstants(RuntimeConstants runtimeConstants);

    bool hasRuntimeConstants() const;

    const boost::optional<RuntimeConstants>& getRuntimeConstants() const;
    const boost::optional<BSONObj>& getLet() const;

    const write_ops::WriteCommandBase& getWriteCommandBase() const;
    void setWriteCommandBase(write_ops::WriteCommandBase writeCommandBase);

    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;
    std::string toString() const;

    void setAllowImplicitCreate(bool doAllow) {
        _allowImplicitCollectionCreation = doAllow;
    }

    bool isImplicitCreateAllowed() const {
        return _allowImplicitCollectionCreation;
    }

    /**
     * Generates a new request, the same as the old, but with insert _ids if required.
     */
    static BatchedCommandRequest cloneInsertWithIds(BatchedCommandRequest origCmdRequest);

    /** These are used to return empty refs from Insert ops that don't carry runtimeConstants
     * or let parameters in getLet and getRuntimeConstants.
     */
    const static boost::optional<RuntimeConstants> kEmptyRuntimeConstants;
    const static boost::optional<BSONObj> kEmptyLet;

private:
    template <typename Req, typename F, typename... As>
    static decltype(auto) _visitImpl(Req&& r, F&& f, As&&... as) {
        switch (r._batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                return std::forward<F>(f)(*r._insertReq, std::forward<As>(as)...);
            case BatchedCommandRequest::BatchType_Update:
                return std::forward<F>(f)(*r._updateReq, std::forward<As>(as)...);
            case BatchedCommandRequest::BatchType_Delete:
                return std::forward<F>(f)(*r._deleteReq, std::forward<As>(as)...);
        }
        MONGO_UNREACHABLE;
    }
    template <typename... As>
    decltype(auto) _visit(As&&... as) {
        return _visitImpl(*this, std::forward<As>(as)...);
    }
    template <typename... As>
    decltype(auto) _visit(As&&... as) const {
        return _visitImpl(*this, std::forward<As>(as)...);
    }

    BatchType _batchType;

    std::unique_ptr<write_ops::Insert> _insertReq;
    std::unique_ptr<write_ops::Update> _updateReq;
    std::unique_ptr<write_ops::Delete> _deleteReq;

    boost::optional<ChunkVersion> _shardVersion;
    boost::optional<DatabaseVersion> _dbVersion;

    boost::optional<BSONObj> _writeConcern;
    bool _allowImplicitCollectionCreation = true;
};

/**
 * Similar to above, this class wraps the write items of a command request into a generically usable
 * type. Very thin wrapper, does not own the write item itself.
 */
class BatchItemRef {
public:
    BatchItemRef(const BatchedCommandRequest* request, int index);

    BatchedCommandRequest::BatchType getOpType() const {
        return _request.getBatchType();
    }

    int getItemIndex() const {
        return _index;
    }

    const auto& getDocument() const {
        return _request.getInsertRequest().getDocuments()[_index];
    }
    const auto& getUpdate() const {
        return _request.getUpdateRequest().getUpdates()[_index];
    }

    const auto& getDelete() const {
        return _request.getDeleteRequest().getDeletes()[_index];
    }

    auto& getLet() const {
        return _request.getLet();
    }

    auto& getRuntimeConstants() const {
        return _request.getRuntimeConstants();
    }

private:
    const BatchedCommandRequest& _request;
    const int _index;
};

}  // namespace mongo

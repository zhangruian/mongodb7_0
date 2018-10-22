// biggie_recovery_unit.h


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

#include <vector>

#include "mongo/db/record_id.h"
#include "mongo/db/storage/biggie/biggie_kv_engine.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace biggie {

class RecoveryUnit : public ::mongo::RecoveryUnit {
    stdx::function<void()> _waitUntilDurableCallback;
    // Official master is kept by KVEngine
    KVEngine* _KVEngine;
    bool _dirty = false;  // Whether or not we have written to this _workingCopy.
    std::shared_ptr<StringStore> _mergeBase;
    std::unique_ptr<StringStore> _workingCopy;

public:
    RecoveryUnit(KVEngine* parentKVEngine, stdx::function<void()> cb = nullptr);

    void beginUnitOfWork(OperationContext* opCtx) override final;
    void commitUnitOfWork() override final;
    void abortUnitOfWork() override final;

    virtual bool waitUntilDurable() override;

    virtual void abandonSnapshot() override;

    virtual void registerChange(Change* change) override;

    virtual SnapshotId getSnapshotId() const override;

    virtual void setOrderedCommit(bool orderedCommit) override;

    // Biggie specific function declarations below.
    StringStore* getWorkingCopy() {
        return _workingCopy.get();
    }
    inline void makeDirty() {
        _dirty = true;
    }
    /**
     * Checks if there already exists a current working copy and merge base; if not fetches
     * one and creates them.
     */
    bool forkIfNeeded();

private:
    typedef std::shared_ptr<Change> ChangePtr;
    typedef std::vector<ChangePtr> Changes;

    Changes _changes;
};

}  // namespace biggie
}  // namespace mongo

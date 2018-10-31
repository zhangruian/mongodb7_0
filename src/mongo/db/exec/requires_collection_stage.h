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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * A base class for plan stages which access a collection. In addition to providing derived classes
 * access to the Collection pointer, the primary purpose of this class is to assume responsibility
 * for checking that the collection is still valid (e.g. has not been dropped) when recovering from
 * yield.
 *
 * Subclasses must implement the saveStage() and restoreState() variants tagged with RequiresCollTag
 * in order to supply custom yield preparation or yield recovery logic.
 */
class RequiresCollectionStage : public PlanStage {
public:
    RequiresCollectionStage(const char* stageType, OperationContext* opCtx, const Collection* coll)
        : PlanStage(stageType, opCtx),
          _collection(coll),
          _collectionUUID(_collection->uuid().get()) {}

    virtual ~RequiresCollectionStage() = default;

protected:
    struct RequiresCollTag {};

    void doSaveState() final;

    void doRestoreState() final;

    /**
     * Performs yield preparation specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void saveState(RequiresCollTag) = 0;

    /**
     * Performs yield recovery specific to a stage which subclasses from RequiresCollectionStage.
     */
    virtual void restoreState(RequiresCollTag) = 0;

    const Collection* collection() {
        return _collection;
    }

    UUID uuid() {
        return _collectionUUID;
    }

private:
    const Collection* _collection;
    const UUID _collectionUUID;
};

}  // namespace mongo

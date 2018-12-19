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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;

static const NamespaceString nss("unittests.PlanExecutorInvalidationTest");

/**
 * Test fixture for verifying that plan executors correctly raise errors when invalidating events
 * such as collection or index drops happen during yield.
 */
class PlanExecutorInvalidationTest : public unittest::Test {
public:
    PlanExecutorInvalidationTest() : _client(&_opCtx) {
        _ctx.reset(new dbtests::WriteContextForTests(&_opCtx, nss.ns()));
        _client.dropCollection(nss.ns());

        for (int i = 0; i < N(); ++i) {
            _client.insert(nss.ns(), BSON("foo" << i));
        }
    }

    /**
     * Return a plan executor that is going over the collection in nss.ns().
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getCollscan() {
        unique_ptr<WorkingSet> ws(new WorkingSet());
        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;
        unique_ptr<CollectionScan> scan(
            new CollectionScan(&_opCtx, collection(), params, ws.get(), NULL));

        // Create a plan executor to hold it
        auto qr = stdx::make_unique<QueryRequest>(nss);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(qr));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Takes ownership of 'ws', 'scan', and 'cq'.
        auto statusWithPlanExecutor = PlanExecutor::make(&_opCtx,
                                                         std::move(ws),
                                                         std::move(scan),
                                                         std::move(cq),
                                                         _ctx->db()->getCollection(&_opCtx, nss),
                                                         PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        return std::move(statusWithPlanExecutor.getValue());
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeIxscanPlan(BSONObj keyPattern,
                                                                        BSONObj startKey,
                                                                        BSONObj endKey) {
        auto indexDescriptor =
            collection()->getIndexCatalog()->findIndexByKeyPatternAndCollationSpec(
                &_opCtx, keyPattern, {});
        ASSERT(indexDescriptor);
        return InternalPlanner::indexScan(&_opCtx,
                                          collection(),
                                          indexDescriptor,
                                          startKey,
                                          endKey,
                                          BoundInclusion::kIncludeBothStartAndEndKeys,
                                          PlanExecutor::YIELD_MANUAL);
    }

    int N() {
        return 50;
    }

    Collection* collection() {
        return _ctx->db()->getCollection(&_opCtx, nss);
    }

    // Order of these is important for initialization
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    unique_ptr<dbtests::WriteContextForTests> _ctx;
    DBDirectClient _client;
};

TEST_F(PlanExecutorInvalidationTest, ExecutorToleratesDeletedDocumentsDuringYield) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();

    // Delete some data, namely the next 2 things we'd expect.
    _client.remove(nss.ns(), BSON("foo" << 10));
    _client.remove(nss.ns(), BSON("foo" << 11));

    exec->restoreState();

    // Make sure that the PlanExecutor moved forward over the deleted data.  We don't see foo==10 or
    // foo==11.
    for (int i = 12; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    ASSERT_EQUALS(PlanExecutor::IS_EOF, exec->getNext(&obj, NULL));
}

TEST_F(PlanExecutorInvalidationTest, PlanExecutorThrowsOnRestoreWhenCollectionIsDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();

    // Drop a collection that's not ours.
    _client.dropCollection("unittests.someboguscollection");

    exec->restoreState();

    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
    ASSERT_EQUALS(10, obj["foo"].numberInt());

    exec->saveState();

    _client.dropCollection(nss.ns());

    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, CollScanExecutorDoesNotDieWhenAllIndicesDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), BSON("foo" << 1)));

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    _client.dropIndexes(nss.ns());
    exec->restoreState();

    // Read the rest of the collection.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, CollScanExecutorDoesNotDieWhenOneIndexDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), BSON("foo" << 1)));

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();
    _client.dropIndex(nss.ns(), BSON("foo" << 1));
    exec->restoreState();

    // Read the rest of the collection.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorDiesWhenAllIndexesDropped) {
    BSONObj keyPattern = BSON("foo" << 1);
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), keyPattern));

    // Create a second index which is not used by the plan executor.
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), BSON("bar" << 1)));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop the index which the plan executor is scanning while the executor is in a saved state.
    exec->saveState();
    _client.dropIndexes(nss.ns());

    // Restoring the executor should throw.
    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorDiesWhenIndexBeingScannedIsDropped) {
    BSONObj keyPattern = BSON("foo" << 1);
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), keyPattern));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop all indexes while the executor is saved.
    exec->saveState();
    _client.dropIndex(nss.ns(), keyPattern);

    // Restoring the executor should throw.
    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, IxscanExecutorSurvivesWhenUnrelatedIndexIsDropped) {
    BSONObj keyPatternFoo = BSON("foo" << 1);
    BSONObj keyPatternBar = BSON("bar" << 1);
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), keyPatternFoo));
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), keyPatternBar));

    auto exec = makeIxscanPlan(keyPatternFoo, BSON("foo" << 0), BSON("foo" << N()));

    // Start scanning the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Drop an index which the plan executor is *not* scanning while the executor is in a saved
    // state.
    exec->saveState();
    _client.dropIndex(nss.ns(), keyPatternBar);
    exec->restoreState();

    // Scan the rest of the index.
    for (int i = 10; i < N(); ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }
}

TEST_F(PlanExecutorInvalidationTest, ExecutorThrowsOnRestoreWhenDatabaseIsDropped) {
    auto exec = getCollscan();
    BSONObj obj;

    // Read some of it.
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    exec->saveState();

    // Drop a DB that's not ours.  We can't have a lock at all to do this as dropping a DB
    // requires a "global write lock."
    _ctx.reset();
    _client.dropDatabase("somesillydb");
    _ctx.reset(new dbtests::WriteContextForTests(&_opCtx, nss.ns()));
    exec->restoreState();

    ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
    ASSERT_EQUALS(10, obj["foo"].numberInt());

    exec->saveState();

    // Drop our DB.  Once again, must give up the lock.
    _ctx.reset();
    _client.dropDatabase("unittests");
    _ctx.reset(new dbtests::WriteContextForTests(&_opCtx, nss.ns()));
    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

// TODO SERVER-31695: Allow PlanExecutors to remain valid after collection rename.
TEST_F(PlanExecutorInvalidationTest, CollScanDiesOnCollectionRenameWithinDatabase) {
    auto exec = getCollscan();

    // Partially scan the collection.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    // Rename the collection.
    exec->saveState();
    BSONObj info;
    ASSERT_TRUE(_client.runCommand("admin",
                                   BSON("renameCollection" << nss.ns() << "to"
                                                           << "unittests.new_collection_name"
                                                           << "dropTarget"
                                                           << true),
                                   info));

    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

// TODO SERVER-31695: Allow PlanExecutors to remain valid after collection rename.
TEST_F(PlanExecutorInvalidationTest, IxscanDiesOnCollectionRenameWithinDatabase) {
    BSONObj keyPattern = BSON("foo" << 1);
    ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), keyPattern));

    auto exec = makeIxscanPlan(keyPattern, BSON("foo" << 0), BSON("foo" << N()));

    // Partially scan the index.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj.firstElement().numberInt());
    }

    // Rename the collection.
    exec->saveState();
    BSONObj info;
    ASSERT_TRUE(_client.runCommand("admin",
                                   BSON("renameCollection" << nss.ns() << "to"
                                                           << "unittests.new_collection_name"
                                                           << "dropTarget"
                                                           << true),
                                   info));

    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

TEST_F(PlanExecutorInvalidationTest, CollScanDiesOnRestartCatalog) {
    auto exec = getCollscan();

    // Partially scan the collection.
    BSONObj obj;
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&obj, NULL));
        ASSERT_EQUALS(i, obj["foo"].numberInt());
    }

    // Restart the catalog during yield. Verify that yield recovery throws with the expected error
    // code.
    exec->saveState();
    BSONObj info;
    ASSERT_TRUE(_client.runCommand("admin", BSON("restartCatalog" << 1), info));
    ASSERT_THROWS_CODE(exec->restoreState(), DBException, ErrorCodes::QueryPlanKilled);
}

}  // namespace mongo

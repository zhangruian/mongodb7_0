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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/util/background.h"

#include "mongo/logv2/log.h"

namespace mongo {

namespace {

/**
 * All the information needed to run dbCheck on a single collection.
 */
struct DbCheckCollectionInfo {
    NamespaceString nss;
    BSONKey start;
    BSONKey end;
    int64_t maxCount;
    int64_t maxSize;
    int64_t maxRate;
    int64_t maxDocsPerBatch;
    int64_t maxBytesPerBatch;
    int64_t maxBatchTimeMillis;
};

/**
 * A run of dbCheck consists of a series of collections.
 */
using DbCheckRun = std::vector<DbCheckCollectionInfo>;

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const std::string& dbName,
                                                const DbCheckSingleInvocation& invocation) {
    NamespaceString nss(dbName, invocation.getColl());
    AutoGetCollectionForRead agc(opCtx, nss);

    uassert(ErrorCodes::NamespaceNotFound,
            "Collection " + invocation.getColl() + " not found",
            agc.getCollection());

    uassert(40619,
            "Cannot run dbCheck on " + nss.toString() + " because it is not replicated",
            nss.isReplicated());

    const auto start = invocation.getMinKey();
    const auto end = invocation.getMaxKey();
    const auto maxCount = invocation.getMaxCount();
    const auto maxSize = invocation.getMaxSize();
    const auto maxRate = invocation.getMaxCountPerSecond();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBytesPerBatch = invocation.getMaxBytesPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();
    const auto info = DbCheckCollectionInfo{nss,
                                            start,
                                            end,
                                            maxCount,
                                            maxSize,
                                            maxRate,
                                            maxDocsPerBatch,
                                            maxBytesPerBatch,
                                            maxBatchTimeMillis};
    auto result = std::make_unique<DbCheckRun>();
    result->push_back(info);
    return result;
}

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const std::string& dbName,
                                            const DbCheckAllInvocation& invocation) {
    uassert(
        ErrorCodes::InvalidNamespace, "Cannot run dbCheck on local database", dbName != "local");

    AutoGetDb agd(opCtx, StringData(dbName), MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound, "Database " + dbName + " not found", agd.getDb());

    const int64_t max = std::numeric_limits<int64_t>::max();
    const auto rate = invocation.getMaxCountPerSecond();
    const auto maxDocsPerBatch = invocation.getMaxDocsPerBatch();
    const auto maxBytesPerBatch = invocation.getMaxBytesPerBatch();
    const auto maxBatchTimeMillis = invocation.getMaxBatchTimeMillis();
    auto result = std::make_unique<DbCheckRun>();
    auto perCollectionWork = [&](const CollectionPtr& coll) {
        DbCheckCollectionInfo info{coll->ns(),
                                   BSONKey::min(),
                                   BSONKey::max(),
                                   max,
                                   max,
                                   rate,
                                   maxDocsPerBatch,
                                   maxBytesPerBatch,
                                   maxBatchTimeMillis};
        result->push_back(info);
        return true;
    };
    mongo::catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, perCollectionWork);

    return result;
}


/**
 * Factory function for producing DbCheckRun's from command objects.
 */
std::unique_ptr<DbCheckRun> getRun(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const BSONObj& obj) {
    BSONObjBuilder builder;

    // Get rid of generic command fields.
    for (const auto& elem : obj) {
        if (!isGenericArgument(elem.fieldNameStringData())) {
            builder.append(elem);
        }
    }

    BSONObj toParse = builder.obj();

    // If the dbCheck argument is a string, this is the per-collection form.
    if (toParse["dbCheck"].type() == BSONType::String) {
        return singleCollectionRun(
            opCtx, dbName, DbCheckSingleInvocation::parse(IDLParserErrorContext(""), toParse));
    } else {
        // Otherwise, it's the database-wide form.
        return fullDatabaseRun(
            opCtx, dbName, DbCheckAllInvocation::parse(IDLParserErrorContext(""), toParse));
    }
}


/**
 * The BackgroundJob in which dbCheck actually executes on the primary.
 */
class DbCheckJob : public BackgroundJob {
public:
    DbCheckJob(const StringData& dbName, std::unique_ptr<DbCheckRun> run)
        : BackgroundJob(true), _done(false), _dbName(dbName.toString()), _run(std::move(run)) {}

protected:
    virtual std::string name() const override {
        return "dbCheck";
    }

    virtual void run() override {
        // Every dbCheck runs in its own client.
        ThreadClient tc(name(), getGlobalServiceContext());

        for (const auto& coll : *_run) {
            try {
                _doCollection(coll);
            } catch (const DBException& e) {
                auto logEntry = dbCheckErrorHealthLogEntry(
                    coll.nss, "dbCheck failed", OplogEntriesEnum::Batch, e.toStatus());
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*logEntry);
                return;
            }

            if (_done) {
                LOGV2(20451, "dbCheck terminated due to stepdown");
                return;
            }
        }
    }

private:
    void _doCollection(const DbCheckCollectionInfo& info) {
        // The collection was confirmed as existing in singleCollectionRun().
        // runBatch() will handle the case of the collection having been dropped since then.

        if (_done) {
            return;
        }

        // Parameters for the hasher.
        auto start = info.start;
        bool reachedEnd = false;

        // Make sure the totals over all of our batches don't exceed the provided limits.
        int64_t totalBytesSeen = 0;
        int64_t totalDocsSeen = 0;

        // Limit the rate of the check.
        using Clock = stdx::chrono::system_clock;
        using TimePoint = stdx::chrono::time_point<Clock>;
        TimePoint lastStart = Clock::now();
        int64_t docsInCurrentInterval = 0;

        do {
            using namespace std::literals::chrono_literals;

            if (Clock::now() - lastStart > 1s) {
                lastStart = Clock::now();
                docsInCurrentInterval = 0;
            }

            auto result = _runBatch(info, start, info.maxDocsPerBatch, info.maxBytesPerBatch);

            if (_done) {
                return;
            }

            std::unique_ptr<HealthLogEntry> entry;

            if (!result.isOK()) {
                entry = dbCheckErrorHealthLogEntry(
                    info.nss, "dbCheck batch failed", OplogEntriesEnum::Batch, result.getStatus());
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*entry);
                return;
            } else {
                auto stats = result.getValue();
                entry = dbCheckBatchEntry(info.nss,
                                          stats.nDocs,
                                          stats.nBytes,
                                          stats.md5,
                                          stats.md5,
                                          start,
                                          stats.lastKey,
                                          stats.time);
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*entry);
            }

            auto stats = result.getValue();

            start = stats.lastKey;

            // Update our running totals.
            totalDocsSeen += stats.nDocs;
            totalBytesSeen += stats.nBytes;
            docsInCurrentInterval += stats.nDocs;

            // Check if we've exceeded any limits.
            bool reachedLast = stats.lastKey >= info.end;
            bool tooManyDocs = totalDocsSeen >= info.maxCount;
            bool tooManyBytes = totalBytesSeen >= info.maxSize;
            reachedEnd = reachedLast || tooManyDocs || tooManyBytes;

            if (docsInCurrentInterval > info.maxRate && info.maxRate > 0) {
                // If an extremely low max rate has been set (substantially smaller than the batch
                // size) we might want to sleep for multiple seconds between batches.
                int64_t timesExceeded = docsInCurrentInterval / info.maxRate;

                stdx::this_thread::sleep_for(timesExceeded * 1s - (Clock::now() - lastStart));
            }
        } while (!reachedEnd);
    }

    /**
     * For organizing the results of batches.
     */
    struct BatchStats {
        int64_t nDocs;
        int64_t nBytes;
        BSONKey lastKey;
        std::string md5;
        repl::OpTime time;
    };

    // Set if the job cannot proceed.
    bool _done;
    std::string _dbName;
    std::unique_ptr<DbCheckRun> _run;

    StatusWith<BatchStats> _runBatch(const DbCheckCollectionInfo& info,
                                     const BSONKey& first,
                                     int64_t batchDocs,
                                     int64_t batchBytes) {
        // New OperationContext for each batch.
        auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();
        DbCheckOplogBatch batch;

        // Acquire collection lock in S mode.
        AutoGetCollection coll(opCtx, info.nss, MODE_S);
        const auto& collection = coll.getCollection();
        if (_stepdownHasOccurred(opCtx, info.nss)) {
            _done = true;
            return Status(ErrorCodes::PrimarySteppedDown, "dbCheck terminated due to stepdown");
        }

        if (!collection) {
            const auto msg = "Collection under dbCheck no longer exists";
            auto entry = dbCheckHealthLogEntry(info.nss,
                                               SeverityEnum::Info,
                                               "dbCheck failed",
                                               OplogEntriesEnum::Batch,
                                               BSON("success" << false << "error" << msg));
            HealthLog::get(opCtx).log(*entry);
            return {ErrorCodes::NamespaceNotFound, msg};
        }

        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           collection,
                           first,
                           info.end,
                           std::min(batchDocs, info.maxCount),
                           std::min(batchBytes, info.maxSize));
        } catch (const DBException& e) {
            return e.toStatus();
        }

        const auto deadline = Date_t::now() + Milliseconds(info.maxBatchTimeMillis);
        Status status = hasher->hashAll(opCtx, deadline);

        if (!status.isOK()) {
            return status;
        }

        std::string md5 = hasher->total();

        batch.setType(OplogEntriesEnum::Batch);
        batch.setNss(info.nss);
        batch.setMd5(md5);
        batch.setMinKey(first);
        batch.setMaxKey(BSONKey(hasher->lastKey()));

        BatchStats result;

        // Send information on this batch over the oplog.
        result.time = _logOp(opCtx, info.nss, collection->uuid(), batch.toBSON());

        result.nDocs = hasher->docsSeen();
        result.nBytes = hasher->bytesSeen();
        result.lastKey = hasher->lastKey();
        result.md5 = md5;

        return result;
    }

    /**
     * Return `true` iff the primary the check is running on has stepped down.
     */
    bool _stepdownHasOccurred(OperationContext* opCtx, const NamespaceString& nss) {
        Status status = opCtx->checkForInterruptNoAssert();

        if (!status.isOK()) {
            return true;
        }

        auto coord = repl::ReplicationCoordinator::get(opCtx);

        if (!coord->canAcceptWritesFor(opCtx, nss)) {
            return true;
        }

        return false;
    }

    repl::OpTime _logOp(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const UUID& uuid,
                        const BSONObj& obj) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(nss);
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(obj);
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
        return writeConflictRetry(
            opCtx, "dbCheck oplog entry", NamespaceString::kRsOplogNamespace.ns(), [&] {
                auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
                oplogEntry.setWallClockTime(clockSource->now());

                WriteUnitOfWork uow(opCtx);
                repl::OpTime result = repl::logOp(opCtx, &oplogEntry);
                uow.commit();
                return result;
            });
    }
};

/**
 * The command, as run on the primary.
 */
class DbCheckCmd : public BasicCommand {
public:
    DbCheckCmd() : BasicCommand("dbCheck") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Validate replica set consistency.\n"
               "Invoke with { dbCheck: <collection name/uuid>,\n"
               "              minKey: <first key, exclusive>,\n"
               "              maxKey: <last key, inclusive>,\n"
               "              maxCount: <max number of docs>,\n"
               "              maxSize: <max size of docs>,\n"
               "              maxCountPerSecond: <max rate in docs/sec> } "
               "              maxDocsPerBatch: <max number of docs/batch> } "
               "              maxBytesPerBatch: <try to keep a batch within max bytes/batch> } "
               "              maxBatchTimeMillis: <max time processing a batch in milliseconds> } "
               "to check a collection.\n"
               "Invoke with {dbCheck: 1} to check all collections in the database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        const bool isAuthorized =
            AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forAnyResource(), ActionType::dbCheck);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto job = getRun(opCtx, dbname, cmdObj);
        try {
            (new DbCheckJob(dbname, std::move(job)))->go();
        } catch (const DBException& e) {
            result.append("ok", false);
            result.append("err", e.toString());
            return false;
        }
        result.append("ok", true);
        return true;
    }
} dbCheckCmd;

}  // namespace
}  // namespace mongo

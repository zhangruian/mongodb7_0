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

#include <queue>

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo {
class BucketCatalog {
    struct ExecutionStats;
    class MinMax;

public:
    class Bucket;

    enum class CombineWithInsertsFromOtherClients {
        kAllow,
        kDisallow,
    };

    struct CommitInfo {
        StatusWith<SingleWriteResult> result;
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    /**
     * The basic unit of work for a bucket. Each insert will return a shared_ptr to a WriteBatch.
     * When a writer is finished with all their insertions, they should then take steps to ensure
     * each batch they wrote into is committed. To ensure a batch is committed, a writer should
     * first attempt to claimCommitRights(). If successful, the writer can proceed to commit (or
     * abort) the batch via BucketCatalog::prepareCommit and BucketCatalog::finish. If unsuccessful,
     * it means another writer is in the process of committing. The writer can proceed to do other
     * work (like commit another batch), and when they have no other work to do, they can wait for
     * this batch to be committed by executing the blocking operation getResult().
     */
    class WriteBatch {
        friend class BucketCatalog;

    public:
        WriteBatch() = delete;

        WriteBatch(Bucket* bucket, const UUID& lsid, const std::shared_ptr<ExecutionStats>& stats);

        /**
         * Attempt to claim the right to commit (or abort) a batch. If it returns true, rights are
         * granted. If it returns false, rights are revoked, and the caller should get the result
         * of the batch with getResult(). Non-blocking.
         */
        bool claimCommitRights();

        /**
         * Retrieve the result of the write batch commit. Should be called by any interested party
         * that does not have commit rights. Blocking.
         */
        StatusWith<CommitInfo> getResult() const;

        Bucket* bucket() const;

        const std::vector<BSONObj>& measurements() const;
        const BSONObj& min() const;
        const BSONObj& max() const;
        const StringSet& newFieldNamesToBeInserted() const;
        uint32_t numPreviouslyCommittedMeasurements() const;

        /**
         * Whether the batch is active and can be written to.
         */
        bool active() const;

        /**
         * Whether the batch has been committed or aborted.
         */
        bool finished() const;

        BSONObj toBSON() const;

    private:
        /**
         * Add a measurement. Active batches only.
         */
        void _addMeasurement(const BSONObj& doc);

        /**
         * Record a set of new-to-the-bucket fields. Active batches only.
         */
        void _recordNewFields(StringSet&& fields);

        /**
         * Prepare the batch for commit. Sets min/max appropriately, records the number of documents
         * that have previously been committed to the bucket, and marks the batch inactive. Must
         * have commit rights.
         */
        void _prepareCommit();

        /**
         * Report the result and status of a commit, and notify anyone waiting on getResult(). Must
         * have commit rights. Inactive batches only.
         */
        void _finish(const CommitInfo& info);

        /**
         * Abandon the write batch and notify any waiters that the bucket has been cleared. Must
         * have commit rights.
         */
        void _abort();


        Bucket* _bucket;
        const UUID _lsid;
        std::shared_ptr<ExecutionStats> _stats;

        std::vector<BSONObj> _measurements;
        BSONObj _min;  // Batch-local min; full if first batch, updates otherwise.
        BSONObj _max;  // Batch-local max; full if first batch, updates otherwise.
        uint32_t _numPreviouslyCommittedMeasurements;
        StringSet _newFieldNamesToBeInserted;

        bool _active = true;

        AtomicWord<bool> _commitRights{false};
        SharedPromise<CommitInfo> _promise;
    };

    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() = default;

    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    /**
     * Returns the metadata for the given bucket in the following format:
     *     {<metadata field name>: <value>}
     * All measurements in the given bucket share same metadata value.
     *
     * Returns an empty document if the given bucket cannot be found or if this time-series
     * collection was not created with a metadata field name.
     */
    BSONObj getMetadata(Bucket* bucket) const;

    /**
     * Returns the WriteBatch into which the document was inserted. Any caller who receives the same
     * batch may commit or abort the batch after claiming commit rights. See WriteBatch for more
     * details.
     */
    StatusWith<std::shared_ptr<WriteBatch>> insert(OperationContext* opCtx,
                                                   const NamespaceString& ns,
                                                   const BSONObj& doc,
                                                   CombineWithInsertsFromOtherClients combine);

    /**
     * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
     * commit rights on batch.
     */
    void prepareCommit(std::shared_ptr<WriteBatch> batch);

    /**
     * Records the result of a batch commit. Caller must already have commit rights on batch, and
     * batch must have been previously prepared.
     */
    void finish(std::shared_ptr<WriteBatch> batch, const CommitInfo& info);

    /**
     * Aborts the given write batch and any other outstanding batches on the same bucket. Caller
     * must already have commit rights on batch, and batch must not be finished.
     */
    void abort(std::shared_ptr<WriteBatch> batch);

    /**
     * Clears the buckets for the given namespace.
     */
    void clear(const NamespaceString& ns);

    /**
     * Clears the buckets for the given database.
     */
    void clear(StringData dbName);

    /**
     * Appends the execution stats for the given namespace to the builder.
     */
    void appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const;

private:
    /**
     * This class provides a mutex with shared and exclusive locking semantics. Unlike some shared
     * mutex implementations, it does not allow for writer starvation (assuming the underlying
     * Mutex implemenation does not allow for starvation). The underlying mechanism is simply an
     * array of Mutex instances. To take a shared lock, a thread's ID is hashed, mapping the thread
     * to a particular mutex, which is then locked. To take an exclusive lock, all mutexes are
     * locked.
     *
     * This behavior makes it easy to allow concurrent read access while still allowing writes to
     * occur safely with exclusive access. It should only be used for situations where observed
     * access patterns are read-mostly.
     *
     * A shared lock *cannot* be upgraded to an exclusive lock.
     */
    class StripedMutex {
    public:
        static constexpr std::size_t kNumStripes = 16;
        StripedMutex() = default;

        using SharedLock = stdx::unique_lock<Mutex>;
        SharedLock lockShared() const;

        class ExclusiveLock {
        public:
            ExclusiveLock() = default;
            explicit ExclusiveLock(const StripedMutex&);

        private:
            std::array<stdx::unique_lock<Mutex>, kNumStripes> _locks;
        };
        ExclusiveLock lockExclusive() const;

    private:
        mutable std::array<Mutex, kNumStripes> _mutexes;
    };

    struct BucketMetadata {
    public:
        BucketMetadata() = default;
        BucketMetadata(BSONObj&& obj, std::shared_ptr<const ViewDefinition>& view);

        bool operator==(const BucketMetadata& other) const;

        const BSONObj& toBSON() const;

        StringData getMetaField() const;

        const CollatorInterface* getCollator() const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(std::move(h),
                              std::hash<std::string_view>()(std::string_view(
                                  metadata._sorted.objdata(), metadata._sorted.objsize())));
        }

    private:
        BSONObj _metadata;
        std::shared_ptr<const ViewDefinition> _view;

        // This stores the _metadata object with all fields sorted to allow for binary comparisons.
        BSONObj _sorted;
    };

    class MinMax {
    public:
        /*
         * Updates the min/max according to 'comp', ignoring the 'metaField' field.
         */
        void update(const BSONObj& doc,
                    boost::optional<StringData> metaField,
                    const StringData::ComparatorInterface* stringComparator,
                    const std::function<bool(int, int)>& comp);

        /**
         * Returns the full min/max object.
         */
        BSONObj toBSON() const;

        /**
         * Returns the updates since the previous time this function was called in the format for
         * an update op.
         */
        BSONObj getUpdates();

        /*
         * Returns the approximate memory usage of this MinMax.
         */
        uint64_t getMemoryUsage() const;

    private:
        enum class Type {
            kObject,
            kArray,
            kValue,
            kUnset,
        };

        void _update(BSONElement elem,
                     const StringData::ComparatorInterface* stringComparator,
                     const std::function<bool(int, int)>& comp);
        void _updateWithMemoryUsage(MinMax* minMax,
                                    BSONElement elem,
                                    const StringData::ComparatorInterface* stringComparator,
                                    const std::function<bool(int, int)>& comp);

        void _append(BSONObjBuilder* builder) const;
        void _append(BSONArrayBuilder* builder) const;

        /*
         * Appends updates, if any, to the builder. Returns whether any updates were appended by
         * this MinMax or any MinMaxes below it.
         */
        bool _appendUpdates(BSONObjBuilder* builder);

        /*
         * Clears the '_updated' flag on this MinMax and all MinMaxes below it.
         */
        void _clearUpdated();

        StringMap<MinMax> _object;
        std::vector<MinMax> _array;
        BSONObj _value;

        Type _type = Type::kUnset;

        bool _updated = false;

        uint64_t _memoryUsage = 0;
    };

    using IdleList = std::list<Bucket*>;

public:
    class Bucket {
    public:
        friend class BucketCatalog;

        /**
         * Returns the ID for the underlying bucket.
         */
        const OID& id() const;

        /**
         * Returns whether all measurements have been committed.
         */
        bool allCommitted() const;

    private:
        /**
         * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
         * to overflow, we will create a new bucket and recalculate the change to the bucket size
         * and data fields.
         */
        void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                                 boost::optional<StringData> metaField,
                                                 StringSet* newFieldNamesToBeInserted,
                                                 uint32_t* newFieldNamesSize,
                                                 uint32_t* sizeToBeAdded) const;

        /**
         * Returns whether BucketCatalog::commit has been called at least once on this bucket.
         */
        bool _hasBeenCommitted() const;

        /**
         * Return a pointer to the current, open batch.
         */
        std::shared_ptr<WriteBatch> _activeBatch(const UUID& lsid,
                                                 const std::shared_ptr<ExecutionStats>& stats);

        // Access to the bucket is controlled by this lock
        mutable Mutex _mutex;

        // The bucket ID for the underlying document
        OID _id = OID::gen();

        // The namespace that this bucket is used for.
        NamespaceString _ns;

        // The metadata of the data that this bucket contains.
        BucketMetadata _metadata;

        // Top-level field names of the measurements that have been inserted into the bucket.
        StringSet _fieldNames;

        // The minimum values for each field in the bucket.
        MinMax _min;

        // The maximum values for each field in the bucket.
        MinMax _max;

        // The latest time that has been inserted into the bucket.
        Date_t _latestTime;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        uint64_t _size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint32_t _numMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint32_t _numCommittedMeasurements = 0;

        // Whether the bucket is full. This can be due to number of measurements, size, or time
        // range.
        bool _full = false;

        // The batch that has been prepared and is currently in the process of being committed, if
        // any.
        std::shared_ptr<WriteBatch> _preparedBatch;

        // Batches, per logical session, that haven't been committed or aborted yet.
        stdx::unordered_map<UUID, std::shared_ptr<WriteBatch>, UUID::Hash> _batches;

        // If the bucket is in the _idleList, then its position is recorded here.
        boost::optional<IdleList::iterator> _idleListEntry = boost::none;

        // Approximate memory usage of this bucket.
        uint64_t _memoryUsage = sizeof(*this);
    };

private:
    struct ExecutionStats {
        AtomicWord<long long> numBucketInserts;
        AtomicWord<long long> numBucketUpdates;
        AtomicWord<long long> numBucketsOpenedDueToMetadata;
        AtomicWord<long long> numBucketsClosedDueToCount;
        AtomicWord<long long> numBucketsClosedDueToSize;
        AtomicWord<long long> numBucketsClosedDueToTimeForward;
        AtomicWord<long long> numBucketsClosedDueToTimeBackward;
        AtomicWord<long long> numBucketsClosedDueToMemoryThreshold;
        AtomicWord<long long> numCommits;
        AtomicWord<long long> numWaits;
        AtomicWord<long long> numMeasurementsCommitted;
    };

    /**
     * Helper class to handle all the locking necessary to lookup and lock a bucket for use. This
     * is intended primarily for using a single bucket, including replacing it when it becomes full.
     * If the usage pattern iterates over several buckets, you will instead want to use raw access
     * using the different mutexes with the locking semantics described below.
     */
    class BucketAccess {
    public:
        BucketAccess() = delete;
        BucketAccess(BucketCatalog* catalog,
                     const std::tuple<NamespaceString, BucketMetadata>& key,
                     ExecutionStats* stats,
                     const Date_t& time);
        BucketAccess(BucketCatalog* catalog, Bucket* bucket);
        ~BucketAccess();

        bool isLocked() const;
        Bucket* operator->();
        operator bool() const;
        operator Bucket*() const;

        // Release the bucket lock, typically in order to reacquire the catalog lock.
        void release();

        /**
         * Close the existing, full bucket and open a new one for the same metadata.
         * Parameter is a function which should check that the bucket is indeed still full after
         * reacquiring the necessary locks. The first parameter will give the function access to
         * this BucketAccess instance, with the bucket locked.
         */
        void rollover(const std::function<bool(BucketAccess*)>& isBucketFull);

        // Adjust the time associated with the bucket (id) if it hasn't been committed yet.
        void setTime();

        // Retrieve the time associated with the bucket (id)
        Date_t getTime() const;

    private:
        // Helper to find and lock an open bucket for the given metadata if it exists. Requires a
        // shared lock on the catalog. Returns true if the bucket exists and was locked.
        bool _findOpenBucketAndLock(std::size_t hash);

        // Helper to find an open bucket for the given metadata if it exists, create it if it
        // doesn't, and lock it. Requires an exclusive lock on the catalog.
        void _findOrCreateOpenBucketAndLock(std::size_t hash);

        // Lock _bucket.
        void _acquire();

        // Allocate a new bucket in the catalog, set the local state to that bucket, and aquire
        // a lock on it.
        void _create(bool openedDuetoMetadata = true);

        BucketCatalog* _catalog;
        const std::tuple<NamespaceString, BucketMetadata>* _key = nullptr;
        ExecutionStats* _stats = nullptr;
        const Date_t* _time = nullptr;

        Bucket* _bucket;
        stdx::unique_lock<Mutex> _guard;
    };

    class ServerStatus;

    StripedMutex::SharedLock _lockShared() const;
    StripedMutex::ExclusiveLock _lockExclusive() const;

    void _waitToCommitBatch(const std::shared_ptr<WriteBatch>& batch);

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    bool _removeBucket(Bucket* bucket, bool bucketIsUnused);

    /**
     * Adds the bucket to a list of idle buckets to be expired at a later date
     */
    void _markBucketIdle(Bucket* bucket);

    /**
     * Remove the bucket from the list of idle buckets
     */
    void _markBucketNotIdle(Bucket* bucket);

    /**
     * Verify the bucket is currently unused by taking a lock on it. Must hold exclusive lock from
     * the outside for the result to be meaningful.
     */
    void _verifyBucketIsUnused(Bucket* bucket) const;

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
     */
    void _expireIdleBuckets(ExecutionStats* stats);

    std::size_t _numberOfIdleBuckets() const;

    // Allocate a new bucket (and ID) and add it to the catalog
    Bucket* _allocateBucket(const std::tuple<NamespaceString, BucketMetadata>& key,
                            const Date_t& time,
                            ExecutionStats* stats,
                            bool openedDuetoMetadata);

    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns);
    const std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    void _setIdTimestamp(Bucket* bucket, const Date_t& time);

    /**
     * You must hold a lock on _bucketMutex when accessing _allBuckets or _openBuckets.
     * While holding a lock on _bucketMutex, you can take a lock on an individual bucket, then
     * release _bucketMutex. Any iterators on the protected structures should be considered invalid
     * once the lock is released. Any subsequent access to the structures requires relocking
     * _bucketMutex. You must *not* be holding a lock on a bucket when you attempt to acquire the
     * lock on _mutex, as this can result in deadlock.
     *
     * The StripedMutex class has both shared (read-only) and exclusive (write) locks. If you are
     * going to write to any of the protected structures, you must hold an exclusive lock.
     *
     * Typically, if you want to acquire a bucket, you should use the BucketAccess RAII
     * class to do so, as it will take care of most of this logic for you. Only use the _bucketMutex
     * directly for more global maintenance where you want to take the lock once and interact with
     * multiple buckets atomically.
     */
    mutable StripedMutex _bucketMutex;

    // All buckets currently in the catalog, including buckets which are full but not yet committed.
    stdx::unordered_set<std::unique_ptr<Bucket>> _allBuckets;

    // The current open bucket for each namespace and metadata pair.
    stdx::unordered_map<std::tuple<NamespaceString, BucketMetadata>, Bucket*> _openBuckets;

    // This mutex protects access to _idleBuckets
    mutable Mutex _idleMutex = MONGO_MAKE_LATCH("BucketCatalog::_idleMutex");

    // Buckets that do not have any writers.
    IdleList _idleBuckets;

    /**
     * This mutex protects access to the _executionStats map. Once you complete your lookup, you
     * can keep the shared_ptr to an individual namespace's stats object and release the lock. The
     * object itself is thread-safe (using atomics).
     */
    mutable StripedMutex _statsMutex;

    // Per-namespace execution stats.
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // A placeholder to be returned in case a namespace has no allocated statistics object
    static const std::shared_ptr<ExecutionStats> kEmptyStats;

    // Counter for buckets created by the bucket catalog.
    uint64_t _bucketNum = 0;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;
};
}  // namespace mongo

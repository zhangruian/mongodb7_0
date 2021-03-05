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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/util/string_map.h"

#include <queue>

namespace mongo {
class BucketCatalog {
public:
    // This constant, together with parameters defined in timeseries.idl, defines limits on the
    // measurements held in a bucket.
    static constexpr auto kTimeseriesBucketMaxTimeRange = Hours(1);

    class BucketId {
    public:
        const OID& operator*() const;
        const OID* operator->() const;

        bool operator==(const BucketId& other) const;
        bool operator!=(const BucketId& other) const;
        bool operator<(const BucketId& other) const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketId& bucketId) {
            return H::combine(std::move(h), bucketId._num);
        }

    protected:
        BucketId(uint64_t num) : _num(num) {}

        std::shared_ptr<OID> _id{std::make_shared<OID>(OID::gen())};

    private:
        uint64_t _num;
    };

    struct CommitInfo {
        StatusWith<SingleWriteResult> result;
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    struct InsertResult {
        BucketId bucketId;
        boost::optional<Future<CommitInfo>> commitInfo;
    };

    struct CommitData {
        std::vector<BSONObj> docs;
        BSONObj bucketMin;  // The full min/max if this is the bucket's first commit, or the updates
        BSONObj bucketMax;  // since the previous commit if not.
        uint32_t numCommittedMeasurements;
        StringSet newFieldNamesToBeInserted;

        BSONObj toBSON() const;
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
    BSONObj getMetadata(const BucketId& bucketId) const;

    /**
     * Returns the id of the bucket that the document belongs in, and a Future to wait on if the
     * caller is a waiter for the bucket. If no Future is provided, the caller is the committer for
     * this bucket.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const BSONObj& doc);

    /**
     * Returns the uncommitted measurements and the number of measurements that have already been
     * committed for the given bucket. This should be called continuously by the committer until
     * there are no more uncommitted measurements.
     */
    CommitData commit(const BucketId& bucketId,
                      boost::optional<CommitInfo> previousCommitInfo = boost::none);

    /**
     * Clears the given bucket.
     */
    void clear(const BucketId& bucketId);

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

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(std::move(h), metadata._keyString.hash());
        }

    private:
        BSONObj _metadata;
        std::shared_ptr<const ViewDefinition> _view;

        // This encodes the _metadata object with all fields sorted in collation order
        KeyString::Value _keyString;
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

    struct Bucket {
        // Access to the bucket is controlled by this lock
        Mutex lock;

        // The namespace that this bucket is used for.
        NamespaceString ns;

        // The metadata of the data that this bucket contains.
        BucketMetadata metadata;

        // Measurements to be inserted into the bucket.
        std::vector<BSONObj> measurementsToBeInserted;

        // New top-level field names of the measurements to be inserted.
        StringSet newFieldNamesToBeInserted;

        // Top-level field names of the measurements that have been inserted into the bucket.
        StringSet fieldNames;

        // The minimum values for each field in the bucket.
        MinMax min;

        // The maximum values for each field in the bucket.
        MinMax max;

        // The latest time that has been inserted into the bucket.
        Date_t latestTime;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        uint64_t size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint32_t numMeasurements = 0;

        // The number of measurements that were most recently returned from a call to commit().
        uint32_t numPendingCommitMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint32_t numCommittedMeasurements = 0;

        // The number of current writers for the bucket.
        uint32_t numWriters = 0;

        // Promises for committers to fulfill in order to signal to waiters that their measurements
        // have been committed.
        std::queue<boost::optional<Promise<CommitInfo>>> promises;

        // Whether the bucket is full. This can be due to number of measurements, size, or time
        // range.
        bool full = false;

        // Approximate memory usage of this bucket.
        uint64_t memoryUsage = sizeof(*this);

        /**
         * Determines the effect of adding 'doc' to this bucket If adding 'doc' causes this bucket
         * to overflow, we will create a new bucket and recalculate the change to the bucket size
         * and data fields.
         */
        void calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                                boost::optional<StringData> metaField,
                                                StringSet* newFieldNamesToBeInserted,
                                                uint32_t* newFieldNamesSize,
                                                uint32_t* sizeToBeAdded) const;

        /**
         * Returns whether BucketCatalog::commit has been called on this bucket.
         */
        bool hasBeenCommitted() const;
    };

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

    // a wrapper around the numerical id to allow timestamp manipulations
    class BucketIdInternal : public BucketId {
    public:
        static BucketIdInternal min();

        BucketIdInternal(const Date_t& time, uint64_t num);

        Date_t getTime() const;
        void setTime(const Date_t& time);
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
        BucketAccess(BucketCatalog* catalog, const BucketId& bucketId);
        ~BucketAccess();

        bool isLocked() const;
        Bucket* operator->();
        operator bool() const;
        operator std::shared_ptr<Bucket>() const;

        // Release the bucket lock, typically in order to reacquire the catalog lock.
        void release();

        /**
         * Close the existing, full bucket and open a new one for the same metadata.
         * Parameter is a function which should check that the bucket is indeed still full after
         * reacquiring the necessary locks. The first parameter will give the function access to
         * this BucketAccess instance, with the bucket locked.
         */
        void rollover(const std::function<bool(BucketAccess*)>& isBucketFull);

        // Retrieve the (safely cached) id of the bucket.
        const BucketIdInternal& id();

        // Adjust the time associated with the bucket (id) if it hasn't been committed yet.
        void setTime();

    private:
        // Constructor helper to find and lock an existing bucket. Takes a shared lock on the
        // catalog.
        bool _findAndLockExisting(std::size_t hash);

        // Constructor helper to find a bucket if it exists, create it if it doesn't, and lock it.
        // Takes an exclusive lock on the catalog.
        void _findOrCreateAndLock(std::size_t hash);

        // Lock _bucket.
        void _acquire();

        // Allocate a new bucket and add it to the catalog with the given ID.
        void _create(const BucketId& id);

        BucketCatalog* _catalog;
        const std::tuple<NamespaceString, BucketMetadata>* _key = nullptr;
        ExecutionStats* _stats = nullptr;
        const Date_t* _time = nullptr;

        BucketIdInternal _id = BucketIdInternal::min();
        std::shared_ptr<Bucket> _bucket;
        stdx::unique_lock<Mutex> _guard;
    };

    class ServerStatus;

    using NsBuckets = std::set<std::tuple<NamespaceString, BucketId>>;
    using IdleBuckets = std::set<BucketId>;

    StripedMutex::SharedLock _lockShared() const;
    StripedMutex::ExclusiveLock _lockExclusive() const;

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    void _removeBucket(const BucketId& bucketId,
                       boost::optional<NsBuckets::iterator> nsBucketsIt = boost::none,
                       boost::optional<IdleBuckets::iterator> idleBucketsIt = boost::none);

    void _markBucketIdle(const BucketId& bucketId);
    void _markBucketNotIdle(const BucketId& bucketId);
    void _markBucketNotIdle(const IdleBuckets::iterator& it);

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
     */
    void _expireIdleBuckets(ExecutionStats* stats);

    std::size_t _numberOfIdleBuckets() const;

    /**
     * Creates a new (internal) bucket ID to identify a bucket.
     */
    BucketIdInternal _createNewBucketId(const Date_t& time, ExecutionStats* stats);

    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns);
    const std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    /**
     * You must hold a lock on _stripedMutex when accessing _buckets, _bucketIds, or _nsBuckets.
     * While holding a lock on _stripedMutex, you can take a lock on an individual bucket, then
     * release _stripedMutex. Any iterators on the protected structures should be considered invalid
     * once the lock is released. Any subsequent access to the structures requires relocking
     * _stripedMutex. You must *not* be holding a lock on a bucket when you attempt to acquire the
     * lock on _mutex, as this can result in deadlock.
     *
     * The StripedMutex class has both shared (read-only) and exclusive (write) locks. If you are
     * going to write to any of the three protected structures, you must hold an exclusive lock.
     *
     * Typically, if you want to acquire a bucket, you should use the BucketAccess RAII
     * class to do so, as it will take care of most of this logic for you. Only use the _mutex
     * directly for more global maintenance where you want to take the lock once and interact with
     * multiple buckets atomically.
     */
    mutable StripedMutex _stripedMutex;

    // All buckets currently in the catalog, including buckets which are full but not yet committed.
    stdx::unordered_map<BucketId, std::shared_ptr<Bucket>> _buckets;

    // The _id of the current bucket for each namespace and metadata pair.
    stdx::unordered_map<std::tuple<NamespaceString, BucketMetadata>, BucketIdInternal> _bucketIds;

    // All buckets ordered by their namespaces.
    NsBuckets _nsBuckets;

    // This mutex protects access to _idleBuckets
    mutable Mutex _idleBucketsLock = MONGO_MAKE_LATCH("BucketCatalog::_idleBucketsLock");

    // Buckets that do not have any writers.
    IdleBuckets _idleBuckets;

    /**
     * This mutex protects access to the _executionStats map. Once you complete your lookup, you
     * can keep the shared_ptr to an individual namespace's stats object and release the lock. The
     * object itself is thread-safe (atomics).
     */
    mutable Mutex _executionStatsLock = MONGO_MAKE_LATCH("BucketCatalog::_executionStatsLock");

    // Per-collection execution stats.
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // A placeholder to be returned in case a namespace has no allocated statistics object
    static const std::shared_ptr<ExecutionStats> kEmptyStats;

    // Counter for buckets created by the bucket catalog.
    uint64_t _bucketNum = 0;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;
};
}  // namespace mongo

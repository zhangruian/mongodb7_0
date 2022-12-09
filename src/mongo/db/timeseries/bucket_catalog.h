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

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/flat_bson.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BucketCatalog {
protected:
    // Number of new field names we can hold in NewFieldNames without needing to allocate memory.
    static constexpr std::size_t kNumStaticNewFields = 10;
    using NewFieldNames = boost::container::small_vector<StringMapHashedKey, kNumStaticNewFields>;

    // Number of measurements we can hold in a batch without needing to allocate memory.
    static constexpr std::size_t kNumStaticBatchMeasurements = 10;
    using BatchMeasurements = boost::container::small_vector<BSONObj, kNumStaticBatchMeasurements>;

    using StripeNumber = std::uint8_t;

    using EraCountMap = std::map<uint64_t, uint64_t>;

    using ShouldClearFn = std::function<bool(const NamespaceString&)>;

    class BucketStateManager;

    /**
     * Identifier to lookup bucket by namespace and OID, with pre-computed hash.
     */
    struct BucketId {
        using Hash = std::size_t;

        BucketId() = delete;
        BucketId(const NamespaceString& nss, const OID& oid);

        NamespaceString ns;
        OID oid;
        Hash hash;

        bool operator==(const BucketId& other) const {
            return oid == other.oid && ns == other.ns;
        }
        bool operator!=(const BucketId& other) const {
            return !(*this == other);
        }

        template <typename H>
        friend H AbslHashValue(H h, const BucketId& bucketId) {
            return H::combine(std::move(h), bucketId.oid, bucketId.ns);
        }
    };

    struct BucketHandle {
        const BucketId bucketId;
        const StripeNumber stripe;
    };

    struct ExecutionStats {
        AtomicWord<long long> numBucketInserts;
        AtomicWord<long long> numBucketUpdates;
        AtomicWord<long long> numBucketsOpenedDueToMetadata;
        AtomicWord<long long> numBucketsClosedDueToCount;
        AtomicWord<long long> numBucketsClosedDueToSchemaChange;
        AtomicWord<long long> numBucketsClosedDueToSize;
        AtomicWord<long long> numBucketsClosedDueToCachePressure;
        AtomicWord<long long> numBucketsClosedDueToTimeForward;
        AtomicWord<long long> numBucketsClosedDueToTimeBackward;
        AtomicWord<long long> numBucketsClosedDueToMemoryThreshold;
        AtomicWord<long long> numBucketsClosedDueToReopening;
        AtomicWord<long long> numBucketsArchivedDueToMemoryThreshold;
        AtomicWord<long long> numBucketsArchivedDueToTimeBackward;
        AtomicWord<long long> numCommits;
        AtomicWord<long long> numWaits;
        AtomicWord<long long> numMeasurementsCommitted;
        AtomicWord<long long> numBucketsReopened;
        AtomicWord<long long> numBucketsKeptOpenDueToLargeMeasurements;
        AtomicWord<long long> numBucketsFetched;
        AtomicWord<long long> numBucketsQueried;
        AtomicWord<long long> numBucketFetchesFailed;
        AtomicWord<long long> numBucketQueriesFailed;
        AtomicWord<long long> numBucketReopeningsFailed;
        AtomicWord<long long> numDuplicateBucketsReopened;
    };

    class ExecutionStatsController {
    public:
        ExecutionStatsController(const std::shared_ptr<ExecutionStats>& collectionStats,
                                 ExecutionStats* globalStats)
            : _collectionStats(collectionStats), _globalStats(globalStats) {}

        ExecutionStatsController() = delete;

        void incNumBucketInserts(long long increment = 1);
        void incNumBucketUpdates(long long increment = 1);
        void incNumBucketsOpenedDueToMetadata(long long increment = 1);
        void incNumBucketsClosedDueToCount(long long increment = 1);
        void incNumBucketsClosedDueToSchemaChange(long long increment = 1);
        void incNumBucketsClosedDueToSize(long long increment = 1);
        void incNumBucketsClosedDueToCachePressure(long long increment = 1);
        void incNumBucketsClosedDueToTimeForward(long long increment = 1);
        void incNumBucketsClosedDueToTimeBackward(long long increment = 1);
        void incNumBucketsClosedDueToMemoryThreshold(long long increment = 1);
        void incNumBucketsClosedDueToReopening(long long increment = 1);
        void incNumBucketsArchivedDueToMemoryThreshold(long long increment = 1);
        void incNumBucketsArchivedDueToTimeBackward(long long increment = 1);
        void incNumCommits(long long increment = 1);
        void incNumWaits(long long increment = 1);
        void incNumMeasurementsCommitted(long long increment = 1);
        void incNumBucketsReopened(long long increment = 1);
        void incNumBucketsKeptOpenDueToLargeMeasurements(long long increment = 1);
        void incNumBucketsFetched(long long increment = 1);
        void incNumBucketsQueried(long long increment = 1);
        void incNumBucketFetchesFailed(long long increment = 1);
        void incNumBucketQueriesFailed(long long increment = 1);
        void incNumBucketReopeningsFailed(long long increment = 1);
        void incNumDuplicateBucketsReopened(long long increment = 1);

    private:
        std::shared_ptr<ExecutionStats> _collectionStats;
        ExecutionStats* _globalStats;
    };

    class Bucket;
    struct CreationInfo;
    struct Stripe;

public:
    enum class CombineWithInsertsFromOtherClients {
        kAllow,
        kDisallow,
    };

    struct CommitInfo {
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    /**
     * Information of a Bucket that got closed while performing an operation on this BucketCatalog.
     * The object is move-only--when it is destructed, it will notify the BucketCatalog that we are
     * done compressing the bucket (or have decided not to) and it can forget about the bucket's
     * state, making it eligible for reopening.
     */
    class ClosedBucket {
    public:
        ClosedBucket() = default;
        ~ClosedBucket();
        ClosedBucket(BucketStateManager*,
                     const BucketId&,
                     const std::string&,
                     boost::optional<uint32_t>,
                     bool);
        ClosedBucket(ClosedBucket&&);
        ClosedBucket& operator=(ClosedBucket&&);
        ClosedBucket(const ClosedBucket&) = delete;
        ClosedBucket& operator=(const ClosedBucket&) = delete;

        BucketId bucketId;
        std::string timeField;
        boost::optional<uint32_t> numMeasurements;
        bool eligibleForReopening = false;

    private:
        BucketStateManager* _bucketStateManager = nullptr;
    };
    using ClosedBuckets = std::vector<ClosedBucket>;

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

        WriteBatch(const BucketHandle& bucketId, OperationId opId, ExecutionStatsController& stats);

        /**
         * Attempts to claim the right to commit a batch. If it returns true, rights are
         * granted. If it returns false, rights are revoked, and the caller should get the result
         * of the batch with getResult(). Non-blocking.
         */
        bool claimCommitRights();

        /**
         * Retrieves the result of the write batch commit. Should be called by any interested party
         * that does not have commit rights. Blocking.
         */
        StatusWith<CommitInfo> getResult();

        /**
         * Returns a handle which can be used by the BucketCatalog internally to locate its record
         * for this bucket.
         */
        const BucketHandle& bucket() const;

        const BatchMeasurements& measurements() const;
        const BSONObj& min() const;
        const BSONObj& max() const;
        const StringMap<std::size_t>& newFieldNamesToBeInserted() const;
        uint32_t numPreviouslyCommittedMeasurements() const;
        bool needToDecompressBucketBeforeInserting() const;

        /**
         * Returns whether the batch has already been committed or aborted.
         */
        bool finished() const;

        BSONObj toBSON() const;

    private:
        /**
         * Adds a measurement. Active batches only.
         */
        void _addMeasurement(const BSONObj& doc);

        /**
         * Records a set of new-to-the-bucket fields. Active batches only.
         */
        void _recordNewFields(Bucket* bucket, NewFieldNames&& fields);

        /**
         * Prepares the batch for commit. Sets min/max appropriately, records the number of
         * documents that have previously been committed to the bucket, and renders the batch
         * inactive. Must have commit rights.
         */
        void _prepareCommit(Bucket* bucket);

        /**
         * Reports the result and status of a commit, and notifies anyone waiting on getResult().
         * Must have commit rights. Inactive batches only.
         */
        void _finish(const CommitInfo& info);

        /**
         * Abandons the write batch and notifies any waiters that the bucket has been cleared.
         */
        void _abort(const Status& status);

        const BucketHandle _bucket;
        OperationId _opId;
        ExecutionStatsController _stats;

        BatchMeasurements _measurements;
        BSONObj _min;  // Batch-local min; full if first batch, updates otherwise.
        BSONObj _max;  // Batch-local max; full if first batch, updates otherwise.
        uint32_t _numPreviouslyCommittedMeasurements = 0;
        StringMap<std::size_t> _newFieldNamesToBeInserted;    // Value is hash of string key
        bool _needToDecompressBucketBeforeInserting = false;  // Bucket is compressed on-disk.

        AtomicWord<bool> _commitRights{false};
        SharedPromise<CommitInfo> _promise;
    };

    /**
     * Return type for the insert function. See insert() for more information.
     */
    class InsertResult {
    public:
        InsertResult() = default;
        InsertResult(InsertResult&&) = default;
        InsertResult& operator=(InsertResult&&) = default;
        InsertResult(const InsertResult&) = delete;
        InsertResult& operator=(const InsertResult&) = delete;

        std::shared_ptr<WriteBatch> batch;
        ClosedBuckets closedBuckets;
        stdx::variant<std::monostate, OID, BSONObj> candidate;
        uint64_t catalogEra = 0;
    };

    /**
     * Function that should run validation against the bucket to ensure it's a proper bucket
     * document. Typically, this should execute Collection::checkValidation.
     */
    using BucketDocumentValidator =
        std::function<std::pair<Collection::SchemaValidationResult, Status>(OperationContext*,
                                                                            const BSONObj&)>;
    struct BucketToReopen {
        BSONObj bucketDocument;
        BucketDocumentValidator validator;
        uint64_t catalogEra = 0;
    };

    struct BucketFindResult {
        BucketFindResult() {}

        bool fetchedBucket{false};
        bool queriedBucket{false};
        boost::optional<BucketToReopen> bucketToReopen{boost::none};
    };

    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() = default;

    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    /**
     * Reopens a closed bucket into the catalog given the bucket document.
     */
    Status reopenBucket(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const BSONObj& bucketDoc);

    /**
     * Returns the metadata for the given bucket in the following format:
     *     {<metadata field name>: <value>}
     * All measurements in the given bucket share same metadata value.
     *
     * Returns an empty document if the given bucket cannot be found or if this time-series
     * collection was not created with a metadata field name.
     */
    BSONObj getMetadata(const BucketHandle& bucket);

    /**
     * Tries to insert 'doc' into a suitable bucket. If an open bucket is full (or has incompatible
     * schema), but is otherwise suitable, we will close it and open a new bucket. If we find no
     * bucket with matching data and a time range that can accomodate 'doc', we will not open a new
     * bucket, but rather let the caller know to search for an archived or closed bucket that can
     * accomodate 'doc'.
     *
     * If a suitable bucket is found or opened, returns the WriteBatch into which 'doc' was
     * inserted and a list of any buckets that were closed to make space to insert 'doc'. Any
     * caller who receives the same batch may commit or abort the batch after claiming commit
     * rights. See WriteBatch for more details.
     *
     * If no suitable bucket is found or opened, returns an optional bucket ID. If set, the bucket
     * ID corresponds to an archived bucket which should be fetched; otherwise the caller should
     * search for a previously-closed bucket that can accomodate 'doc'. The caller should proceed to
     * call 'insert' to insert 'doc', passing any fetched bucket.
     */
    StatusWith<InsertResult> tryInsert(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const StringData::ComparatorInterface* comparator,
                                       const TimeseriesOptions& options,
                                       const BSONObj& doc,
                                       CombineWithInsertsFromOtherClients combine);

    /**
     * Returns the WriteBatch into which the document was inserted and a list of any buckets that
     * were closed in order to make space to insert the document. Any caller who receives the same
     * batch may commit or abort the batch after claiming commit rights. See WriteBatch for more
     * details.
     *
     * If 'bucketToReopen' is passed, we will reopen that bucket and attempt to add 'doc' to that
     * bucket. Otherwise we will attempt to find a suitable open bucket, or open a new bucket if
     * none exists.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const StringData::ComparatorInterface* comparator,
                                    const TimeseriesOptions& options,
                                    const BSONObj& doc,
                                    CombineWithInsertsFromOtherClients combine,
                                    BucketFindResult bucketFindResult = {});

    /**
     * Prepares a batch for commit, transitioning it to an inactive state. Caller must already have
     * commit rights on batch. Returns OK if the batch was successfully prepared, or a status
     * indicating why the batch was previously aborted by another operation.
     */
    Status prepareCommit(std::shared_ptr<WriteBatch> batch);

    /**
     * Records the result of a batch commit. Caller must already have commit rights on batch, and
     * batch must have been previously prepared.
     *
     * Returns bucket information of a bucket if one was closed.
     */
    boost::optional<ClosedBucket> finish(std::shared_ptr<WriteBatch> batch, const CommitInfo& info);

    /**
     * Aborts the given write batch and any other outstanding batches on the same bucket, using the
     * provided status.
     */
    void abort(std::shared_ptr<WriteBatch> batch, const Status& status);

    /**
     * Notifies the catalog of a direct write (that is, a write not initiated by the BucketCatalog)
     * that will be performed on the bucket document with the specified ID. If there is already an
     * internally-prepared operation on that bucket, this method will throw a
     * 'WriteConflictException'. This should be followed by a call to 'directWriteFinish' after the
     * write has been committed, rolled back, or otherwise finished.
     */
    void directWriteStart(const NamespaceString& ns, const OID& oid);

    /**
     * Notifies the catalog that a pending direct write to the bucket document with the specified ID
     * has finished or been abandoned, and normal operations on the bucket can resume. After this
     * point any in-memory representation of the on-disk bucket data from before the direct write
     * should have been cleared from the catalog, and it may be safely reopened from the on-disk
     * state.
     */
    void directWriteFinish(const NamespaceString& ns, const OID& oid);

    /**
     * Clears any bucket whose namespace satisfies the predicate.
     */
    void clear(ShouldClearFn&& shouldClear);

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

    /**
     * Appends the global execution stats for all namespaces to the builder.
     */
    void appendGlobalExecutionStats(BSONObjBuilder* builder) const;

    /**
     * Appends the global bucket state management stats for all namespaces to the builder.
     */
    void appendStateManagementStats(BSONObjBuilder* builder) const;

    /**
     * Reports the current memory usage.
     */
    long long memoryUsage() const;

protected:
    enum class BucketStateFlag : std::uint8_t {
        // Bucket has a prepared batch outstanding.
        kPrepared = 0b00000001,
        // In-memory representation of the bucket may be out of sync with on-disk data. Bucket
        // should not be inserted into.
        kCleared = 0b00000010,
        // Bucket is effectively closed, but has an outstanding compression operation pending, so it
        // is also not eligible for reopening.
        kPendingCompression = 0b00000100,
        // Bucket is effectively closed, but has an outstanding direct write pending, so it is also
        // not eligible for reopening.
        kPendingDirectWrite = 0b00001000,
        // Bucket state is stored in the catalog for synchronization purposes only, but the actual
        // bucket isn't stored in the catalog, nor is it archived.
        kUntracked = 0b00010000,
    };

    class BucketState {
    public:
        BucketState& setFlag(BucketStateFlag);
        BucketState& unsetFlag(BucketStateFlag);
        BucketState& reset();

        bool isSet(BucketStateFlag) const;
        bool isPrepared() const;
        bool conflictsWithReopening() const;
        bool conflictsWithInsertion() const;

        bool operator==(const BucketState&) const;
        std::string toString() const;

    private:
        std::underlying_type<BucketStateFlag>::type _state = 0;
    };

    struct BucketMetadata {
    public:
        BucketMetadata() = default;
        BucketMetadata(BSONElement elem, const StringData::ComparatorInterface* comparator);

        bool operator==(const BucketMetadata& other) const;
        bool operator!=(const BucketMetadata& other) const;

        const BSONObj& toBSON() const;
        const BSONElement& element() const;

        StringData getMetaField() const;

        const StringData::ComparatorInterface* getComparator() const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(
                std::move(h),
                absl::Hash<absl::string_view>()(absl::string_view(
                    metadata._metadataElement.value(), metadata._metadataElement.valuesize())));
        }

    private:
        // Only the value of '_metadataElement' is used for hashing and comparison.
        BSONElement _metadataElement;

        // Empty if metadata field isn't present, owns a copy otherwise.
        BSONObj _metadata;

        const StringData::ComparatorInterface* _comparator = nullptr;
    };

    /**
     * Key to lookup open Bucket for namespace and metadata, with pre-computed hash.
     */
    struct BucketKey {
        using Hash = std::size_t;

        BucketKey() = delete;
        BucketKey(const NamespaceString& nss, const BucketMetadata& meta);

        NamespaceString ns;
        BucketMetadata metadata;
        Hash hash;

        bool operator==(const BucketKey& other) const {
            return ns == other.ns && metadata == other.metadata;
        }
        bool operator!=(const BucketKey& other) const {
            return !(*this == other);
        }

        template <typename H>
        friend H AbslHashValue(H h, const BucketKey& key) {
            return H::combine(std::move(h), key.ns, key.metadata);
        }
    };

    /**
     * Hasher to support pre-computed hash lookup for BucketKey.
     */
    struct BucketHasher {
        std::size_t operator()(const BucketKey& key) const;
        std::size_t operator()(const BucketId& bucketId) const;
        std::size_t operator()(const BucketKey::Hash& key) const;
    };

    /**
     * Information of a Bucket that got archived while performing an operation on this
     * BucketCatalog.
     */
    struct ArchivedBucket {
        ArchivedBucket() = delete;
        ArchivedBucket(const BucketId& bucketId, const std::string& timeField);

        BucketId bucketId;
        std::string timeField;
    };

    /**
     * Struct to hold a portion of the buckets managed by the catalog.
     *
     * Each of the bucket lists, as well as the buckets themselves, are protected by 'mutex'.
     */
    struct Stripe {
        mutable Mutex mutex =
            MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "BucketCatalog::Stripe::mutex");

        // All buckets currently in the catalog, including buckets which are full but not yet
        // committed.
        stdx::unordered_map<BucketId, std::unique_ptr<Bucket>, BucketHasher> allBuckets;

        // The current open bucket for each namespace and metadata pair.
        stdx::unordered_map<BucketKey, Bucket*, BucketHasher> openBuckets;

        // Buckets that do not have any outstanding writes.
        using IdleList = std::list<Bucket*>;
        IdleList idleBuckets;

        // Buckets that are not currently in the catalog, but which are eligible to receive more
        // measurements. The top-level map is keyed by the hash of the BucketKey, while the stored
        // map is keyed by the bucket's minimum timestamp.
        //
        // We invert the key comparison in the inner map so that we can use lower_bound to
        // efficiently find an archived bucket that is a candidate for an incoming measurement.
        stdx::unordered_map<BucketKey::Hash,
                            std::map<Date_t, ArchivedBucket, std::greater<Date_t>>,
                            BucketHasher>
            archivedBuckets;
    };

    /**
     * Mode enum to determine the rollover type decision for a given bucket.
     */
    enum class RolloverAction {
        kNone,       // Keep bucket open
        kArchive,    // Archive bucket
        kSoftClose,  // Close bucket so it remains eligible for reopening
        kHardClose,  // Permanently close bucket
    };

    /**
     * Reasons why a bucket was rolled over.
     */
    enum class RolloverReason {
        kNone,           // Not actually rolled over
        kTimeForward,    // Measurement time would violate max span for this bucket
        kTimeBackward,   // Measurement time was before bucket min time
        kCount,          // Adding this measurement would violate max count
        kSchemaChange,   // This measurement has a schema incompatible with existing measurements
        kCachePressure,  // System is under cache pressure, and adding this measurement would make
                         // the bucket larger than the dynamic size limit
        kSize,  // Adding this measurement would make the bucket larger than the normal size limit
    };

    /**
     * Bundle of information that 'insert' needs to pass down to helper methods that may create a
     * new bucket.
     */
    struct CreationInfo {
        const BucketKey& key;
        StripeNumber stripe;
        const Date_t& time;
        const TimeseriesOptions& options;
        ExecutionStatsController& stats;
        ClosedBuckets* closedBuckets;
        bool openedDuetoMetadata = true;
    };

    /**
     * A helper class to maintain global state about the catalog era used to support asynchronous
     * 'clear' operations. Provides thread-safety by taking the catalog '_mutex' for all operations.
     */
    class BucketStateManager {
    public:
        using StateChangeFn = std::function<boost::optional<BucketState>(
            boost::optional<BucketState>, std::uint64_t)>;

        explicit BucketStateManager(Mutex* m);

        uint64_t getEra();
        uint64_t getEraAndIncrementCount();
        void decrementCountForEra(uint64_t value);
        uint64_t getCountForEra(uint64_t value);

        /**
         * Asynchronously clears all buckets belonging to namespaces satisfying the 'shouldClear'
         * predicate.
         */
        void clearSetOfBuckets(std::function<bool(const NamespaceString&)>&& shouldClear);

        /**
         * Returns the number of clear operations currently stored in the clear registry.
         */
        uint64_t getClearOperationsCount();

        /**
         * Retrieves the bucket state if it is tracked in the catalog. Modifies the bucket state if
         * the bucket is found to have been cleared.
         */
        boost::optional<BucketState> getBucketState(Bucket* bucket);

        /**
         * Retrieves the bucket state if it is tracked in the catalog.
         */
        boost::optional<BucketState> getBucketState(const BucketId& bucketId) const;

        /**
         * Checks whether the bucket has been cleared before changing the bucket state as requested.
         * If the bucket has been cleared, it will set the kCleared flag instead and ignore the
         * requested 'change'. For more details about how the 'change' is processed, see the other
         * variant of this function that takes an 'OID' parameter.
         */
        boost::optional<BucketState> changeBucketState(Bucket* bucket, const StateChangeFn& change);

        /**
         * Changes the bucket state, taking into account the current state, the requested 'change',
         * and allowed state transitions. The return value, if set, is the final state of the bucket
         * with the given ID.
         *
         * If no state is currently tracked for 'id', then the optional input state to 'change' will
         * be 'none'. To initialize the state, 'change' may return a valid `BucketState', and it
         * will be added to the set of tracked states.
         *
         * Similarly, if 'change' returns 'none', the value will be removed from the registry. To
         * perform a noop (i.e. if upon inspecting the input, the change would be invalid), 'change'
         * may simply return its input state unchanged.
         */
        boost::optional<BucketState> changeBucketState(const BucketId& bucketId,
                                                       const StateChangeFn& change);

        /**
         * Appends statistics for observability.
         */
        void appendStats(BSONObjBuilder* builder) const;

    protected:
        void _decrementEraCountHelper(uint64_t era);
        void _incrementEraCountHelper(uint64_t era);
        boost::optional<BucketState> _changeBucketStateHelper(WithLock withLock,
                                                              const BucketId& bucketId,
                                                              const StateChangeFn& change);

        /**
         * Returns whether the Bucket has been marked as cleared by checking against the
         * clearRegistry. Advances Bucket's era up to current global era if the bucket has not been
         * cleared.
         */
        bool _isMemberOfClearedSet(WithLock catalogLock, Bucket* bucket);

        /**
         * A helper function to set the kCleared flag for the given bucket. Results in a noop if the
         * bucket state isn't currently tracked.
         */
        boost::optional<BucketState> _markIndividualBucketCleared(WithLock catalogLock,
                                                                  const BucketId& bucketId);

        /**
         * Removes clear operations from the clear registry that no longer need to be tracked.
         */
        void _cleanClearRegistry();

        // Pointer to 'BucketCatalog::_mutex'.
        Mutex* _mutex;

        // Global number tracking the current number of eras that have passed. Incremented each time
        // a bucket is cleared.
        uint64_t _era;

        // Mapping of era to counts of how many buckets are associated with that era.
        EraCountMap _countMap;

        // Bucket state for synchronization with direct writes
        stdx::unordered_map<BucketId, BucketState, BucketHasher> _bucketStates;

        // Registry storing clear operations. Maps from era to a lambda function which takes in
        // information about a Bucket and returns whether the Bucket has been cleared.
        std::map<uint64_t, ShouldClearFn> _clearRegistry;
    };

    /**
     * The in-memory representation of a time-series bucket document. Maintains all the information
     * needed to add additional measurements, but does not generally store the full contents of the
     * document that have already been committed to disk.
     */

    class Bucket {
    public:
        friend class BucketCatalog;

        Bucket(const BucketId& bucketId,
               StripeNumber stripe,
               BucketKey::Hash keyHash,
               BucketStateManager* bucketStateManager);

        ~Bucket();

        uint64_t getEra() const;

        void setEra(uint64_t era);

        /**
         * Returns the BucketId for the bucket.
         */

        const BucketId& bucketId() const;

        /**
         * Returns the OID for the underlying bucket.
         */
        const OID& oid() const;

        /**
         * Returns the namespace for the underlying bucket.
         */
        const NamespaceString& ns() const;

        /**
         * Returns the number of the stripe that owns the bucket.
         */
        StripeNumber stripe() const;

        /**
         * Returns the pre-computed hash of the corresponding BucketKey.
         */
        BucketKey::Hash keyHash() const;

        /**
         * Returns the time associated with the bucket (id).
         */
        Date_t getTime() const;

        /**
         * Returns the timefield for the underlying bucket.
         */
        StringData getTimeField();

        /**
         * Returns whether all measurements have been committed.
         */
        bool allCommitted() const;

        /**
         * Returns total number of measurements in the bucket.
         */
        uint32_t numMeasurements() const;

        /**
         * Sets the rollover action, to determine what to do with a bucket when all measurements
         * have been committed.
         */
        void setRolloverAction(RolloverAction action);

        /**
         * Determines if the schema for an incoming measurement is incompatible with those already
         * stored in the bucket.
         *
         * Returns true if incompatible
         */
        bool schemaIncompatible(const BSONObj& input,
                                boost::optional<StringData> metaField,
                                const StringData::ComparatorInterface* comparator);

    private:
        /**
         * Determines the effect of adding 'doc' to this bucket. If adding 'doc' causes this bucket
         * to overflow, we will create a new bucket and recalculate the change to the bucket size
         * and data fields.
         */
        void _calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                                 boost::optional<StringData> metaField,
                                                 NewFieldNames* newFieldNamesToBeInserted,
                                                 int32_t* sizeToBeAdded) const;

        /**
         * Returns whether BucketCatalog::commit has been called at least once on this bucket.
         */
        bool _hasBeenCommitted() const;

        /**
         * Return a pointer to the current, open batch.
         */
        std::shared_ptr<WriteBatch> _activeBatch(OperationId opId, ExecutionStatsController& stats);

    protected:
        // The era number of the last log operation the bucket has caught up to
        uint64_t _lastCheckedEra;

        BucketStateManager* _bucketStateManager;

    private:
        // The bucket ID for the underlying document
        const BucketId _bucketId;

        // The stripe which owns this bucket.
        const StripeNumber _stripe;

        // The pre-computed hash of the associated BucketKey
        const BucketKey::Hash _keyHash;

        // The metadata of the data that this bucket contains.
        BucketMetadata _metadata;

        // Top-level hashed field names of the measurements that have been inserted into the bucket.
        StringSet _fieldNames;

        // Top-level hashed new field names that have not yet been committed into the bucket.
        StringSet _uncommittedFieldNames;

        // Time field for the measurements that have been inserted into the bucket.
        std::string _timeField;

        // Minimum timestamp over contained measurements
        Date_t _minTime;

        // The minimum and maximum values for each field in the bucket.
        timeseries::MinMax _minmax;

        // The reference schema for measurements in this bucket. May reflect schema of uncommitted
        // measurements.
        timeseries::Schema _schema;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        int32_t _size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint32_t _numMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint32_t _numCommittedMeasurements = 0;

        // Whether the bucket has been marked for a rollover action. It can be marked for closure
        // due to number of measurements, size, or schema changes, or it can be marked for archival
        // due to time range.
        RolloverAction _rolloverAction = RolloverAction::kNone;

        // Whether this bucket was kept open after exceeding the bucket max size to improve
        // bucketing performance for large measurements.
        bool _keptOpenDueToLargeMeasurements = false;

        // The batch that has been prepared and is currently in the process of being committed, if
        // any.
        std::shared_ptr<WriteBatch> _preparedBatch;

        // Batches, per operation, that haven't been committed or aborted yet.
        stdx::unordered_map<OperationId, std::shared_ptr<WriteBatch>> _batches;

        // If the bucket is in idleBuckets, then its position is recorded here.
        boost::optional<Stripe::IdleList::iterator> _idleListEntry = boost::none;

        // Approximate memory usage of this bucket.
        uint64_t _memoryUsage = sizeof(*this);
    };

    /**
     * Extracts the information from the input 'doc' that is used to map the document to a bucket.
     */
    StatusWith<std::pair<BucketKey, Date_t>> _extractBucketingParameters(
        const NamespaceString& ns,
        const StringData::ComparatorInterface* comparator,
        const TimeseriesOptions& options,
        const BSONObj& doc) const;

    /**
     * Maps bucket key to the stripe that is responsible for it.
     */
    StripeNumber _getStripeNumber(const BucketKey& key) const;

    /**
     * Mode enum to control whether the bucket retrieval methods below will return buckets that have
     * a state that conflicts with insertion.
     */
    enum class IgnoreBucketState { kYes, kNo };

    /**
     * Retrieve a bucket for read-only use.
     */
    const Bucket* _findBucket(const Stripe& stripe,
                              WithLock stripeLock,
                              const BucketId& bucketId,
                              IgnoreBucketState mode = IgnoreBucketState::kNo);

    /**
     * Retrieve a bucket for write use.
     */
    Bucket* _useBucket(Stripe* stripe,
                       WithLock stripeLock,
                       const BucketId& bucketId,
                       IgnoreBucketState mode);

    /**
     * Retrieve a bucket for write use, updating the state in the process.
     */
    Bucket* _useBucketAndChangeState(Stripe* stripe,
                                     WithLock stripeLock,
                                     const BucketId& bucketId,
                                     const BucketStateManager::StateChangeFn& change);

    /**
     * Mode enum to control whether the bucket retrieval methods below will create new buckets if no
     * suitable bucket exists.
     */
    enum class AllowBucketCreation { kYes, kNo };

    /**
     * Retrieve a bucket for write use if one exists. If none exists and 'mode' is set to kYes, then
     * we will create a new bucket.
     */
    Bucket* _useBucket(Stripe* stripe,
                       WithLock stripeLock,
                       const CreationInfo& info,
                       AllowBucketCreation mode);

    /**
     * Given a bucket to reopen, performs validation and constructs the in-memory representation of
     * the bucket. If specified, 'expectedKey' is matched against the key extracted from the
     * document to validate that the bucket is expected (i.e. to help resolve hash collisions for
     * archived buckets). Does *not* hand ownership of the bucket to the catalog.
     */
    StatusWith<std::unique_ptr<Bucket>> _rehydrateBucket(
        OperationContext* opCtx,
        const NamespaceString& ns,
        const StringData::ComparatorInterface* comparator,
        const TimeseriesOptions& options,
        const BucketToReopen& bucketToReopen,
        boost::optional<const BucketKey&> expectedKey);

    /**
     * Given a rehydrated 'bucket', passes ownership of that bucket to the catalog, marking the
     * bucket as open.
     */
    StatusWith<Bucket*> _reopenBucket(Stripe* stripe,
                                      WithLock stripeLock,
                                      ExecutionStatsController stats,
                                      const BucketKey& key,
                                      std::unique_ptr<Bucket>&& bucket,
                                      std::uint64_t targetEra,
                                      ClosedBuckets* closedBuckets);

    /**
     * Check to see if 'insert' can use existing bucket rather than reopening a candidate bucket. If
     * true, chances are the caller raced with another thread to reopen the same bucket, but if
     * false, there might be another bucket that had been cleared, or that has the same _id in a
     * different namespace.
     */
    StatusWith<Bucket*> _reuseExistingBucket(Stripe* stripe,
                                             WithLock stripeLock,
                                             ExecutionStatsController* stats,
                                             const BucketKey& key,
                                             Bucket* existingBucket,
                                             std::uint64_t targetEra);

    /**
     * Helper method to perform the heavy lifting for both 'tryInsert' and 'insert'. See
     * documentation on callers for more details.
     */
    StatusWith<InsertResult> _insert(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     const StringData::ComparatorInterface* comparator,
                                     const TimeseriesOptions& options,
                                     const BSONObj& doc,
                                     CombineWithInsertsFromOtherClients combine,
                                     AllowBucketCreation mode,
                                     BucketFindResult bucketFindResult = {});

    /**
     * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, and
     * 'mode' is set to 'kYes', we will create a new bucket and insert into that bucket.
     */
    stdx::variant<std::shared_ptr<WriteBatch>, RolloverReason> _insertIntoBucket(
        OperationContext* opCtx,
        Stripe* stripe,
        WithLock stripeLock,
        const BSONObj& doc,
        CombineWithInsertsFromOtherClients combine,
        AllowBucketCreation mode,
        CreationInfo* info,
        Bucket* bucket,
        ClosedBuckets* closedBuckets);

    /**
     * Wait for other batches to finish so we can prepare 'batch'
     */
    void _waitToCommitBatch(Stripe* stripe, const std::shared_ptr<WriteBatch>& batch);

    /**
     * Mode to signal to '_removeBucket' what's happening to the bucket, and how to handle the
     * bucket state change.
     */
    enum class RemovalMode {
        kClose,    // Normal closure, pending compression
        kArchive,  // Archive bucket, no state change
        kAbort,    // Bucket is being cleared, possibly due to error, erase state
    };

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    void _removeBucket(Stripe* stripe, WithLock stripeLock, Bucket* bucket, RemovalMode mode);

    /**
     * Archives the given bucket, minimizing the memory footprint but retaining the necessary
     * information required to efficiently identify it as a candidate for future insertions.
     */
    void _archiveBucket(Stripe* stripe,
                        WithLock stripeLock,
                        Bucket* bucket,
                        ClosedBuckets* closedBuckets);

    /**
     * Identifies a previously archived bucket that may be able to accomodate the measurement
     * represented by 'info', if one exists.
     */
    boost::optional<OID> _findArchivedCandidate(Stripe* stripe,
                                                WithLock stripeLock,
                                                const CreationInfo& info);

    /**
     * Identifies a previously archived bucket that may be able to accomodate the measurement
     * represented by 'info', if one exists.
     */
    stdx::variant<std::monostate, OID, BSONObj> _getReopeningCandidate(
        Stripe* stripe,
        WithLock stripeLock,
        const CreationInfo& info,
        bool allowQueryBasedReopening);

    /**
     * Aborts 'batch', and if the corresponding bucket still exists, proceeds to abort any other
     * unprepared batches and remove the bucket from the catalog if there is no unprepared batch.
     */
    void _abort(Stripe* stripe,
                WithLock stripeLock,
                std::shared_ptr<WriteBatch> batch,
                const Status& status);

    /**
     * Aborts any unprepared batches for the given bucket, then removes the bucket if there is no
     * prepared batch. If 'batch' is non-null, it is assumed that the caller has commit rights for
     * that batch.
     */
    void _abort(Stripe* stripe,
                WithLock stripeLock,
                Bucket* bucket,
                std::shared_ptr<WriteBatch> batch,
                const Status& status);

    /**
     * Records that compression for the given bucket has been completed, and the BucketCatalog can
     * forget about the bucket.
     */
    void _compressionDone(const BucketId& bucketId);

    /**
     * Adds the bucket to a list of idle buckets to be expired at a later date.
     */
    void _markBucketIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket);

    /**
     * Remove the bucket from the list of idle buckets. The second parameter encodes whether the
     * caller holds a lock on _idleMutex.
     */
    void _markBucketNotIdle(Stripe* stripe, WithLock stripeLock, Bucket* bucket);

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry
     * threshold.
     */
    void _expireIdleBuckets(Stripe* stripe,
                            WithLock stripeLock,
                            ExecutionStatsController& stats,
                            ClosedBuckets* closedBuckets);

    /**
     * Allocates a new bucket and adds it to the catalog.
     */
    Bucket* _allocateBucket(Stripe* stripe, WithLock stripeLock, const CreationInfo& info);

    /**
     * Determines if 'bucket' needs to be rolled over to accomodate 'doc'. If so, determines whether
     * to archive or close 'bucket'.
     */
    std::pair<RolloverAction, RolloverReason> _determineRolloverAction(
        OperationContext* opCtx,
        const BSONObj& doc,
        CreationInfo* info,
        Bucket* bucket,
        NewFieldNames* newFieldNamesToBeInserted,
        int32_t* sizeToBeAdded,
        AllowBucketCreation mode);

    /**
     * Close the existing, full bucket and open a new one for the same metadata.
     *
     * Writes information about the closed bucket to the 'info' parameter.
     */
    Bucket* _rollover(Stripe* stripe,
                      WithLock stripeLock,
                      Bucket* bucket,
                      const CreationInfo& info,
                      RolloverAction action);

    ExecutionStatsController _getExecutionStats(const NamespaceString& ns);
    std::shared_ptr<ExecutionStats> _getExecutionStats(const NamespaceString& ns) const;

    void _appendExecutionStatsToBuilder(const ExecutionStats* stats, BSONObjBuilder* builder) const;

    /**
     * Calculates the marginal memory usage for an archived bucket. The
     * 'onlyEntryForMatchingMetaHash' parameter indicates that the bucket will be (if inserting)
     * or was (if removing) the only bucket associated with it's meta hash value. If true, then
     * the returned value will attempt to account for the overhead of the map data structure for
     * the meta hash value.
     */
    static long long _marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                                           bool onlyEntryForMatchingMetaHash);

    /**
     * Updates stats to reflect the status of bucket fetches and queries based off of the FindResult
     * (which is populated when attempting to reopen a bucket).
     */
    void _updateBucketFetchAndQueryStats(ExecutionStatsController& stats,
                                         const BucketFindResult& findResult);

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketCatalog::_mutex");

    BucketStateManager _bucketStateManager{&_mutex};

    static constexpr std::size_t kNumberOfStripes = 32;
    std::array<Stripe, kNumberOfStripes> _stripes;

    // Per-namespace execution stats. This map is protected by '_mutex'. Once you complete your
    // lookup, you can keep the shared_ptr to an individual namespace's stats object and release the
    // lock. The object itself is thread-safe (using atomics).
    stdx::unordered_map<NamespaceString, std::shared_ptr<ExecutionStats>> _executionStats;

    // Global execution stats used to report aggregated metrics in server status.
    ExecutionStats _globalExecutionStats;

    // Approximate memory usage of the bucket catalog.
    AtomicWord<uint64_t> _memoryUsage;

    // Approximate cardinality of opened and archived buckets.
    AtomicWord<uint32_t> _numberOfActiveBuckets;

    class ServerStatus;
};
}  // namespace mongo

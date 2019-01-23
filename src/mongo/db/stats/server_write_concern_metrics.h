
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

/**
 * Container for server-wide statistics on writeConcern levels used by operations.
 */
class ServerWriteConcernMetrics {
    MONGO_DISALLOW_COPYING(ServerWriteConcernMetrics);

public:
    ServerWriteConcernMetrics() = default;

    static ServerWriteConcernMetrics* get(ServiceContext* service);
    static ServerWriteConcernMetrics* get(OperationContext* opCtx);

    /**
     * Updates the insert metrics 'numInserts' times according to the 'w' value of
     * 'writeConcernOptions'.
     */
    void recordWriteConcernForInserts(const WriteConcernOptions& writeConcernOptions,
                                      size_t numInserts) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _insertMetrics.recordWriteConcern(writeConcernOptions, numInserts);
    }

    /**
     * Updates the insert metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForInsert(const WriteConcernOptions& writeConcernOptions) {
        recordWriteConcernForInserts(writeConcernOptions, 1);
    }

    /**
     * Updates the update metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForUpdate(const WriteConcernOptions& writeConcernOptions) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _updateMetrics.recordWriteConcern(writeConcernOptions);
    }

    /**
     * Updates the delete metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForDelete(const WriteConcernOptions& writeConcernOptions) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _deleteMetrics.recordWriteConcern(writeConcernOptions);
    }

    BSONObj toBSON() const;

private:
    struct WriteConcernMetricsForOperationType {
        /**
         * Updates counter for the 'w' value of 'writeConcernOptions'.
         */
        void recordWriteConcern(const WriteConcernOptions& writeConcernOptions, size_t numOps = 1);

        void toBSON(BSONObjBuilder* builder) const;

        // Count of operations with writeConcern w:"majority".
        long long wMajorityCount = 0;

        // Count of operations without a writeConcern "w" value.
        long long noWCount = 0;

        // Counts of operations with writeConcern w:<num>.
        std::map<int, long long> wNumCounts;

        // Counts of operations with writeConcern w:"tag".
        StringMap<long long> wTagCounts;
    };

    mutable stdx::mutex _mutex;
    WriteConcernMetricsForOperationType _insertMetrics;
    WriteConcernMetricsForOperationType _updateMetrics;
    WriteConcernMetricsForOperationType _deleteMetrics;
};

}  // namespace mongo

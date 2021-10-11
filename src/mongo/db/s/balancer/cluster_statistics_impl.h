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

#include "mongo/db/s/balancer/balancer_random.h"
#include "mongo/db/s/balancer/cluster_statistics.h"

namespace mongo {

/**
 * Default implementation for the cluster statistics gathering utility. Uses a blocking method to
 * fetch the statistics and does not perform any caching. If any of the shards fails to report
 * statistics fails the entire refresh.
 */
class ClusterStatisticsImpl final : public ClusterStatistics {
public:
    ClusterStatisticsImpl(BalancerRandomSource& random);
    ~ClusterStatisticsImpl();

    StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* opCtx) override;

    StatusWith<std::vector<ShardStatistics>> getCollStats(OperationContext* opCtx,
                                                          NamespaceString const& ns) override;

private:
    StatusWith<std::vector<ShardStatistics>> _getStats(OperationContext* opCtx,
                                                       boost::optional<NamespaceString> ns);

    // Source of randomness when metadata needs to be randomized.
    BalancerRandomSource& _random;
};

}  // namespace mongo

/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/create_indexes_gen.h"
#include "mongo/db/drop_indexes_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo::timeseries {

/**
 * Returns a command object with time-series view namespace translated to bucket namespace.
 */
BSONObj makeTimeseriesCommand(const BSONObj& origCmd,
                              const NamespaceString& ns,
                              StringData nsFieldName);

/*
 * Returns a CreateIndexesCommand for creating indexes on the bucket collection.
 */
CreateIndexesCommand makeTimeseriesCreateIndexesCommand(OperationContext* opCtx,
                                                        const CreateIndexesCommand& origCmd,
                                                        const TimeseriesOptions& options);

/**
 * Returns a DropIndexes for dropping indexes on the bucket collection.
 *
 * The 'index' dropIndexes parameter may refer to an index name, or array of names, or "*" for all
 * indexes, or an index spec key (an object). Only the index spec key has to be translated for the
 * bucket collection. The other forms of 'index' can be passed along unmodified.
 */
DropIndexes makeTimeseriesDropIndexesCommand(OperationContext* opCtx,
                                             const DropIndexes& origCmd,
                                             const TimeseriesOptions& options);

}  // namespace mongo::timeseries

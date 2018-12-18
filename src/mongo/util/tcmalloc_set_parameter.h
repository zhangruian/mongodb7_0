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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"

namespace mongo {

#define TCMALLOC_PARAMETER_LIST(DECL_TCMALLOC_FUNCTIONS)                \
    DECL_TCMALLOC_FUNCTIONS(tcmallocMaxTotalThreadCacheBytes,           \
                            "tcmalloc.max_total_thread_cache_bytes"_sd) \
    DECL_TCMALLOC_FUNCTIONS(tcmallocAggressiveMemoryDecommit,           \
                            "tcmalloc.aggressive_memory_decommit"_sd)

#define DECLARE_TCMALLOC_FUNCTION(XX, YY)                                      \
    Status XX##ServerParameterFromString(StringData str);                      \
    Status XX##ServerParameterSetFromBSON(const BSONElement& newValueElement); \
    void XX##ServerParameterAppendBSON(OperationContext* opCtx, BSONObjBuilder* b, StringData name);

TCMALLOC_PARAMETER_LIST(DECLARE_TCMALLOC_FUNCTION);

}  // namespace mongo

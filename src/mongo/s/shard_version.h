/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/chunk_version_gen.h"

namespace mongo {

/**
 * This class is used to represent the shard version of a collection.
 *
 * It contains the chunk placement information through the ChunkVersion. This class is used for
 * network requests and the shard versioning protocol.
 *
 */
class ShardVersion : public ChunkVersion {
public:
    /**
     * The name for the shard version information field, which shard-aware commands should include
     * if they want to convey shard version.
     */
    static constexpr StringData kShardVersionField = "shardVersion"_sd;

    ShardVersion(ChunkVersion chunkVersion) : ChunkVersion(chunkVersion) {}

    ShardVersion() : ShardVersion(ChunkVersion()) {}

    static ShardVersion parse(const BSONElement& element) {
        auto parsedVersion =
            ChunkVersion60Format::parse(IDLParserContext("ShardVersion"), element.Obj());
        auto version = parsedVersion.getVersion();
        return ShardVersion(ChunkVersion({parsedVersion.getEpoch(), parsedVersion.getTimestamp()},
                                         {version.getSecs(), version.getInc()}));
    }

    void serialize(StringData field, BSONObjBuilder* builder) const {
        ChunkVersion60Format version;
        version.setGeneration({_epoch, _timestamp});
        version.setPlacement(Timestamp(majorVersion(), minorVersion()));
        builder->append(field, version.toBSON());
    }
};

}  // namespace mongo

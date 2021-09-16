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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
namespace write_ops {
// Conservative per array element overhead. This value was calculated as 1 byte (element type) + 5
// bytes (max string encoding of the array index encoded as string and the maximum key is 99999) + 1
// byte (zero terminator) = 7 bytes
constexpr int kWriteCommandBSONArrayPerElementOverheadBytes = 7;

constexpr int kRetryableAndTxnBatchWriteBSONSizeOverhead =
    kWriteCommandBSONArrayPerElementOverheadBytes * 2;

/**
 * Parses the 'limit' property of a delete entry, which has inverted meaning from the 'multi'
 * property of an update.
 *
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
bool readMultiDeleteProperty(const BSONElement& limitElement);

/**
 * Writes the 'isMulti' value as a limit property.
 *
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void writeMultiDeleteProperty(bool isMulti, StringData fieldName, BSONObjBuilder* builder);

/**
 * Serializes the opTime fields to specified BSON builder. A 'term' field will be included only
 * when it is intialized.
 */
void opTimeSerializerWithTermCheck(repl::OpTime opTime, StringData fieldName, BSONObjBuilder* bob);

/**
 * Method to deserialize the specified BSON element to opTime. This method is used by the IDL
 * parser to generate the deserializer code.
 */
repl::OpTime opTimeParser(BSONElement elem);

class UpdateModification {
public:
    enum class Type { kClassic, kPipeline, kDelta, kTransform };
    using TransformFunc = std::function<boost::optional<BSONObj>(const BSONObj&)>;

    /**
     * Used to indicate that a certain type of update is being passed to the constructor.
     */
    struct DiffOptions {
        bool mustCheckExistenceForInsertOperations = true;
    };
    struct ClassicTag {};

    // Given the 'o' field of an update oplog entry, will return an UpdateModification that can be
    // applied. The `options` parameter will be applied only in the case a Delta update is parsed.
    static UpdateModification parseFromOplogEntry(const BSONObj& oField,
                                                  const DiffOptions& options);
    static UpdateModification parseFromClassicUpdate(const BSONObj& modifiers) {
        return UpdateModification(modifiers, ClassicTag{});
    }
    static UpdateModification parseFromV2Delta(const doc_diff::Diff& diff,
                                               DiffOptions const& options) {
        return UpdateModification(diff, options);
    }

    UpdateModification() = default;
    UpdateModification(BSONElement update);
    UpdateModification(std::vector<BSONObj> pipeline);
    UpdateModification(doc_diff::Diff, DiffOptions);
    // Creates an transform-style update. The transform function MUST preserve the _id element.
    UpdateModification(TransformFunc transform);
    // This constructor exists only to provide a fast-path for constructing classic-style updates.
    UpdateModification(const BSONObj& update, ClassicTag);

    /**
     * These methods support IDL parsing of the "u" field from the update command and OP_UPDATE.
     *
     * IMPORTANT: The method should not be modified, as API version input/output guarantees could
     * break because of it.
     */
    static UpdateModification parseFromBSON(BSONElement elem);

    /**
     * IMPORTANT: The method should not be modified, as API version input/output guarantees could
     * break because of it.
     */
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

    int objsize() const;

    Type type() const;

    BSONObj getUpdateClassic() const {
        invariant(type() == Type::kClassic);
        return stdx::get<ClassicUpdate>(_update).bson;
    }

    const std::vector<BSONObj>& getUpdatePipeline() const {
        invariant(type() == Type::kPipeline);
        return stdx::get<PipelineUpdate>(_update);
    }

    doc_diff::Diff getDiff() const {
        invariant(type() == Type::kDelta);
        return stdx::get<DeltaUpdate>(_update).diff;
    }

    const TransformFunc& getTransform() const {
        invariant(type() == Type::kTransform);
        return stdx::get<TransformUpdate>(_update).transform;
    }

    bool mustCheckExistenceForInsertOperations() const {
        invariant(type() == Type::kDelta);
        return stdx::get<DeltaUpdate>(_update).options.mustCheckExistenceForInsertOperations;
    }

    std::string toString() const {
        StringBuilder sb;

        stdx::visit(
            visit_helper::Overloaded{
                [&sb](const ClassicUpdate& classic) {
                    sb << "{type: Classic, update: " << classic.bson << "}";
                },
                [&sb](const PipelineUpdate& pipeline) {
                    sb << "{type: Pipeline, update: " << Value(pipeline).toString() << "}";
                },
                [&sb](const DeltaUpdate& delta) {
                    sb << "{type: Delta, update: " << delta.diff << "}";
                },
                [&sb](const TransformUpdate& transform) { sb << "{type: Transform}"; }},
            _update);

        return sb.str();
    }

private:
    // Wrapper class used to avoid having a variant where multiple alternatives have the same type.
    struct ClassicUpdate {
        BSONObj bson;
    };
    using PipelineUpdate = std::vector<BSONObj>;
    struct DeltaUpdate {
        doc_diff::Diff diff;
        DiffOptions options;
    };
    struct TransformUpdate {
        TransformFunc transform;
    };
    stdx::variant<ClassicUpdate, PipelineUpdate, DeltaUpdate, TransformUpdate> _update;
};

}  // namespace write_ops
}  // namespace mongo

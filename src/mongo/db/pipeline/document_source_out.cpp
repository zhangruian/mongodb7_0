
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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/pipeline/document_source_out_in_place.h"
#include "mongo/db/pipeline/document_source_out_replace_coll.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

std::unique_ptr<DocumentSourceOut::LiteParsed> DocumentSourceOut::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$out stage requires a string or object argument, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::String || spec.type() == BSONType::Object);

    NamespaceString targetNss;
    bool allowSharded;
    WriteModeEnum mode;
    if (spec.type() == BSONType::String) {
        targetNss = NamespaceString(request.getNamespaceString().db(), spec.valueStringData());
        allowSharded = false;
        mode = WriteModeEnum::kModeReplaceCollection;
    } else if (spec.type() == BSONType::Object) {
        auto outSpec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), spec.embeddedObject());

        if (auto targetDb = outSpec.getTargetDb()) {
            targetNss = NamespaceString(*targetDb, outSpec.getTargetCollection());
        } else {
            targetNss =
                NamespaceString(request.getNamespaceString().db(), outSpec.getTargetCollection());
        }

        mode = outSpec.getMode();

        // Sharded output collections are not allowed with mode "replaceCollection".
        allowSharded = mode != WriteModeEnum::kModeReplaceCollection;
    }

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid $out target namespace, " << targetNss.ns(),
            targetNss.isValid());

    // All modes require the "insert" action.
    ActionSet actions{ActionType::insert};
    switch (mode) {
        case WriteModeEnum::kModeReplaceCollection:
            actions.addAction(ActionType::remove);
            break;
        case WriteModeEnum::kModeReplaceDocuments:
            actions.addAction(ActionType::update);
            break;
        case WriteModeEnum::kModeInsertDocuments:
            // "insertDocuments" mode only requires the "insert" action.
            break;
    }

    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{Privilege(ResourcePattern::forExactNamespace(targetNss), actions)};

    return stdx::make_unique<DocumentSourceOut::LiteParsed>(
        std::move(targetNss), std::move(privileges), allowSharded);
}

REGISTER_DOCUMENT_SOURCE(out,
                         DocumentSourceOut::LiteParsed::parse,
                         DocumentSourceOut::createFromBson);

const char* DocumentSourceOut::getSourceName() const {
    return "$out";
}

namespace {
/**
 * Parses the fields of the 'uniqueKey' from the user-specified 'obj' from the $out spec, returning
 * a set of field paths. Throws if 'obj' is invalid.
 */
std::set<FieldPath> parseUniqueKeyFromSpec(const BSONObj& obj) {
    std::set<FieldPath> uniqueKey;
    for (const auto& elem : obj) {
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "All fields of $out uniqueKey must be the number 1, but '"
                              << elem.fieldNameStringData()
                              << "' is of type "
                              << elem.type(),
                elem.isNumber());

        uassert(ErrorCodes::BadValue,
                str::stream() << "All fields of $out uniqueKey must be the number 1, but '"
                              << elem.fieldNameStringData()
                              << "' has the invalid value "
                              << elem.numberDouble(),
                elem.numberDouble() == 1.0);

        const auto res = uniqueKey.insert(FieldPath(elem.fieldNameStringData()));
        uassert(ErrorCodes::BadValue,
                str::stream() << "Found a duplicate field '" << elem.fieldNameStringData()
                              << "' in $out uniqueKey",
                res.second);
    }

    uassert(ErrorCodes::InvalidOptions,
            "If explicitly specifying $out uniqueKey, must include at least one field",
            uniqueKey.size() > 0);
    return uniqueKey;
}

/**
 * Extracts the fields of 'uniqueKey' from 'doc' and returns the key as a BSONObj. Throws if any
 * field of the 'uniqueKey' extracted from 'doc' is nullish or an array.
 */
BSONObj extractUniqueKeyFromDoc(const Document& doc, const std::set<FieldPath>& uniqueKey) {
    MutableDocument result;
    for (const auto& field : uniqueKey) {
        auto value = doc.getNestedField(field);
        uassert(50943,
                str::stream() << "$out write error: uniqueKey field '" << field.fullPath()
                              << "' is an array in the document '"
                              << doc.toString()
                              << "'",
                !value.isArray());
        uassert(
            50905,
            str::stream() << "$out write error: uniqueKey field '" << field.fullPath()
                          << "' cannot be missing, null, undefined or an array. Full document: '"
                          << doc.toString()
                          << "'",
            !value.nullish());
        result.addField(field.fullPath(), std::move(value));
    }
    return result.freeze().toBson();
}
}  // namespace

DocumentSource::GetNextResult DocumentSourceOut::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    if (!_initialized) {
        initializeWriteNs();
        _initialized = true;
    }

    BatchedObjects batch;
    int bufferedBytes = 0;

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        auto doc = nextInput.releaseDocument();

        // Generate an _id if the uniqueKey includes _id but the document doesn't have one.
        if (_uniqueKeyIncludesId && doc.getField("_id"_sd).missing()) {
            MutableDocument mutableDoc(std::move(doc));
            mutableDoc["_id"_sd] = Value(OID::gen());
            doc = mutableDoc.freeze();
        }

        // Extract the unique key before converting the document to BSON.
        auto uniqueKey = extractUniqueKeyFromDoc(doc, _uniqueKeyFields);
        auto insertObj = doc.toBson();

        bufferedBytes += insertObj.objsize();
        if (!batch.empty() &&
            (bufferedBytes > BSONObjMaxUserSize || batch.size() >= write_ops::kMaxWriteBatchSize)) {
            spill(std::move(batch));
            batch.clear();
            bufferedBytes = insertObj.objsize();
        }
        batch.emplace(std::move(insertObj), std::move(uniqueKey));
    }
    if (!batch.empty()) {
        spill(std::move(batch));
        batch.clear();
    }

    switch (nextInput.getStatus()) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case GetNextResult::ReturnStatus::kPauseExecution: {
            return nextInput;  // Propagate the pause.
        }
        case GetNextResult::ReturnStatus::kEOF: {

            finalize();
            _done = true;

            // $out doesn't currently produce any outputs.
            return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

intrusive_ptr<DocumentSourceOut> DocumentSourceOut::create(
    NamespaceString outputNs,
    const intrusive_ptr<ExpressionContext>& expCtx,
    WriteModeEnum mode,
    std::set<FieldPath> uniqueKey) {
    // TODO (SERVER-36832): Allow this combination.
    uassert(
        50939,
        str::stream() << "$out with mode " << WriteMode_serializer(mode)
                      << " is not supported when the output collection is in a different database",
        !(mode == WriteModeEnum::kModeReplaceCollection && outputNs.db() != expCtx->ns.db()));

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "$out cannot be used in a transaction",
            !expCtx->inMultiDocumentTransaction);

    auto readConcernLevel = repl::ReadConcernArgs::get(expCtx->opCtx).getLevel();
    uassert(ErrorCodes::InvalidOptions,
            "$out cannot be used with a 'majority' read concern level",
            readConcernLevel != repl::ReadConcernLevel::kMajorityReadConcern);
    uassert(ErrorCodes::InvalidOptions,
            "$out cannot be used with a 'linearizable' read concern level",
            readConcernLevel != repl::ReadConcernLevel::kLinearizableReadConcern);

    // Although we perform a check for "replaceCollection" mode with a sharded output collection
    // during lite parsing, we need to do it here as well in case mongos is stale or the command is
    // sent directly to the shard.
    uassert(17017,
            str::stream() << "$out with mode " << WriteMode_serializer(mode)
                          << " is not supported to an existing *sharded* output collection.",
            !(mode == WriteModeEnum::kModeReplaceCollection &&
              expCtx->mongoProcessInterface->isSharded(expCtx->opCtx, outputNs)));

    uassert(17385, "Can't $out to special collection: " + outputNs.coll(), !outputNs.isSpecial());

    switch (mode) {
        case WriteModeEnum::kModeReplaceCollection:
            return new DocumentSourceOutReplaceColl(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey));
        case WriteModeEnum::kModeInsertDocuments:
            return new DocumentSourceOutInPlace(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey));
        case WriteModeEnum::kModeReplaceDocuments:
            return new DocumentSourceOutInPlaceReplace(
                std::move(outputNs), expCtx, mode, std::move(uniqueKey));
        default:
            MONGO_UNREACHABLE;
    }
}

DocumentSourceOut::DocumentSourceOut(NamespaceString outputNs,
                                     const intrusive_ptr<ExpressionContext>& expCtx,
                                     WriteModeEnum mode,
                                     std::set<FieldPath> uniqueKey)
    : DocumentSource(expCtx),
      _writeConcern(expCtx->opCtx->getWriteConcern()),
      _done(false),
      _outputNs(std::move(outputNs)),
      _mode(mode),
      _uniqueKeyFields(std::move(uniqueKey)),
      _uniqueKeyIncludesId(_uniqueKeyFields.count("_id") == 1) {}

intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    auto mode = WriteModeEnum::kModeReplaceCollection;
    std::set<FieldPath> uniqueKey;
    NamespaceString outputNs;
    if (elem.type() == BSONType::String) {
        outputNs = NamespaceString(expCtx->ns.db().toString() + '.' + elem.str());
        uniqueKey.emplace("_id");
    } else if (elem.type() == BSONType::Object) {
        auto spec =
            DocumentSourceOutSpec::parse(IDLParserErrorContext("$out"), elem.embeddedObject());

        mode = spec.getMode();

        // Retrieve the target database from the user command, otherwise use the namespace from the
        // expression context.
        if (auto targetDb = spec.getTargetDb()) {
            outputNs = NamespaceString(*targetDb, spec.getTargetCollection());
        } else {
            outputNs = NamespaceString(expCtx->ns.db(), spec.getTargetCollection());
        }

        // Convert unique key object to a vector of FieldPaths.
        std::vector<FieldPath> docKeyPaths = std::get<0>(
            expCtx->mongoProcessInterface->collectDocumentKeyFields(expCtx->opCtx, outputNs));
        std::set<FieldPath> docKeyPathsSet =
            std::set<FieldPath>(std::make_move_iterator(docKeyPaths.begin()),
                                std::make_move_iterator(docKeyPaths.end()));
        if (auto userSpecifiedUniqueKey = spec.getUniqueKey()) {
            uniqueKey = parseUniqueKeyFromSpec(userSpecifiedUniqueKey.get());

            // Skip the unique index check if the provided uniqueKey is the documentKey.
            const bool isDocumentKey = (uniqueKey == docKeyPathsSet);

            // Make sure the uniqueKey has a supporting index. Skip this check if the command is
            // sent from mongos since the uniqueKey check would've happened already.
            uassert(50938,
                    "Cannot find index to verify that $out's unique key will be unique",
                    expCtx->fromMongos || isDocumentKey ||
                        expCtx->mongoProcessInterface->uniqueKeyIsSupportedByIndex(
                            expCtx, outputNs, uniqueKey));
        } else {
            uniqueKey = std::move(docKeyPathsSet);
        }
    } else {
        uasserted(16990,
                  str::stream() << "$out only supports a string or object argument, not "
                                << typeName(elem.type()));
    }

    return create(std::move(outputNs), expCtx, mode, std::move(uniqueKey));
}

Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument serialized(
        Document{{DocumentSourceOutSpec::kTargetCollectionFieldName, _outputNs.coll()},
                 {DocumentSourceOutSpec::kTargetDbFieldName, _outputNs.db()},
                 {DocumentSourceOutSpec::kModeFieldName, WriteMode_serializer(_mode)}});
    BSONObjBuilder uniqueKeyBob;
    for (auto path : _uniqueKeyFields) {
        uniqueKeyBob.append(path.fullPath(), 1);
    }
    serialized[DocumentSourceOutSpec::kUniqueKeyFieldName] = Value(uniqueKeyBob.done());
    return Value(Document{{getSourceName(), serialized.freeze()}});
}

DepsTracker::State DocumentSourceOut::getDependencies(DepsTracker* deps) const {
    deps->needWholeDocument = true;
    return DepsTracker::State::EXHAUSTIVE_ALL;
}
}  // namespace mongo

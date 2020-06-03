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

#include "mongo/db/query/query_request.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

Status checkFieldType(const BSONElement& el, BSONType type) {
    if (type != el.type()) {
        str::stream ss;
        ss << "Failed to parse: " << el.toString() << ". "
           << "'" << el.fieldName() << "' field must be of BSON type " << typeName(type) << ".";
        return Status(ErrorCodes::FailedToParse, ss);
    }

    return Status::OK();
}

}  // namespace

QueryRequest::QueryRequest(NamespaceStringOrUUID nssOrUuid)
    : _nss(nssOrUuid.nss() ? *nssOrUuid.nss() : NamespaceString()), _uuid(nssOrUuid.uuid()) {}

void QueryRequest::refreshNSS(OperationContext* opCtx) {
    if (_uuid) {
        const CollectionCatalog& catalog = CollectionCatalog::get(opCtx);
        auto foundColl = catalog.lookupCollectionByUUID(opCtx, _uuid.get());
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "UUID " << _uuid.get() << " specified in query request not found",
                foundColl);
        dassert(opCtx->lockState()->isDbLockedForMode(foundColl->ns().db(), MODE_IS));
        _nss = foundColl->ns();
    }
    invariant(!_nss.isEmpty());
}

// static
StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::parseFromFindCommand(
    std::unique_ptr<QueryRequest> qr, const BSONObj& cmdObj, bool isExplain) {
    qr->_explain = isExplain;
    bool tailable = false;
    bool awaitData = false;

    // Parse the command BSON by looping through one element at a time.
    BSONObjIterator it(cmdObj);
    while (it.more()) {
        BSONElement el = it.next();
        const auto fieldName = el.fieldNameStringData();
        if (fieldName == kFindCommandName) {
            // Check both UUID and String types for "find" field.
            Status status = checkFieldType(el, BinData);
            if (!status.isOK()) {
                status = checkFieldType(el, String);
            }
            if (!status.isOK()) {
                return status;
            }
        } else if (fieldName == kFilterField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_filter = el.Obj().getOwned();
        } else if (fieldName == kProjectionField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_proj = el.Obj().getOwned();
        } else if (fieldName == kSortField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_sort = el.Obj().getOwned();
        } else if (fieldName == kHintField) {
            BSONObj hintObj;
            if (Object == el.type()) {
                hintObj = cmdObj["hint"].Obj().getOwned();
            } else if (String == el.type()) {
                hintObj = el.wrap("$hint");
            } else {
                return Status(ErrorCodes::FailedToParse,
                              "hint must be either a string or nested object");
            }

            qr->_hint = hintObj;
        } else if (fieldName == repl::ReadConcernArgs::kReadConcernFieldName) {
            // Read concern parsing is handled elsewhere, but we store a copy here.
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_readConcern = el.Obj().getOwned();
        } else if (fieldName == QueryRequest::kUnwrappedReadPrefField) {
            // Read preference parsing is handled elsewhere, but we store a copy here.
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->setUnwrappedReadPref(el.Obj());
        } else if (fieldName == kCollationField) {
            // Collation parsing is handled elsewhere, but we store a copy here.
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_collation = el.Obj().getOwned();
        } else if (fieldName == kSkipField) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'skip' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            long long skip = el.numberLong();

            // A skip value of 0 means that there is no skip.
            if (skip) {
                qr->_skip = skip;
            }
        } else if (fieldName == kLimitField) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'limit' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            long long limit = el.numberLong();

            // A limit value of 0 means that there is no limit.
            if (limit) {
                qr->_limit = limit;
            }
        } else if (fieldName == kBatchSizeField) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'batchSize' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            qr->_batchSize = el.numberLong();
        } else if (fieldName == kNToReturnField) {
            if (!el.isNumber()) {
                str::stream ss;
                ss << "Failed to parse: " << cmdObj.toString() << ". "
                   << "'ntoreturn' field must be numeric.";
                return Status(ErrorCodes::FailedToParse, ss);
            }

            qr->_ntoreturn = el.numberLong();
        } else if (fieldName == kSingleBatchField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_wantMore = !el.boolean();
        } else if (fieldName == kAllowDiskUseField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_allowDiskUse = el.boolean();
        } else if (fieldName == cmdOptionMaxTimeMS) {
            StatusWith<int> maxTimeMS = parseMaxTimeMS(el);
            if (!maxTimeMS.isOK()) {
                return maxTimeMS.getStatus();
            }

            qr->_maxTimeMS = maxTimeMS.getValue();
        } else if (fieldName == kMinField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_min = el.Obj().getOwned();
        } else if (fieldName == kMaxField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            qr->_max = el.Obj().getOwned();
        } else if (fieldName == kReturnKeyField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_returnKey = el.boolean();
        } else if (fieldName == kShowRecordIdField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_showRecordId = el.boolean();
        } else if (fieldName == kTailableField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            tailable = el.boolean();
        } else if (fieldName == kOplogReplayField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            // Ignore the 'oplogReplay' field for compatibility with old clients. Nodes 4.4 and
            // greater will apply the 'oplogReplay' optimization to eligible oplog scans regardless
            // of whether the flag is set explicitly, so the flag is no longer meaningful.
        } else if (fieldName == kNoCursorTimeoutField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_noCursorTimeout = el.boolean();
        } else if (fieldName == kAwaitDataField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            awaitData = el.boolean();
        } else if (fieldName == kPartialResultsField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_allowPartialResults = el.boolean();
        } else if (fieldName == kRuntimeConstantsField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }
            qr->_runtimeConstants =
                RuntimeConstants::parse(IDLParserErrorContext(kRuntimeConstantsField),
                                        cmdObj.getObjectField(kRuntimeConstantsField));
        } else if (fieldName == kLetField) {
            if (auto status = checkFieldType(el, Object); !status.isOK())
                return status;
            qr->_letParameters = el.Obj().getOwned();
        } else if (fieldName == kOptionsField) {
            // 3.0.x versions of the shell may generate an explain of a find command with an
            // 'options' field. We accept this only if the 'options' field is empty so that
            // the shell's explain implementation is forwards compatible.
            //
            // TODO: Remove for 3.4.
            if (!qr->isExplain()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Field '" << kOptionsField
                                            << "' is only allowed for explain.");
            }

            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }

            BSONObj optionsObj = el.Obj();
            if (!optionsObj.isEmpty()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Failed to parse options: " << optionsObj.toString()
                                            << ". You may need to update your shell or driver.");
            }
        } else if (fieldName == kShardVersionField) {
            // Shard version parsing is handled elsewhere.
        } else if (fieldName == kTermField) {
            Status status = checkFieldType(el, NumberLong);
            if (!status.isOK()) {
                return status;
            }
            qr->_replicationTerm = el._numberLong();
        } else if (fieldName == kReadOnceField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }

            qr->_readOnce = el.boolean();
        } else if (fieldName == kAllowSpeculativeMajorityReadField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }
            qr->_allowSpeculativeMajorityRead = el.boolean();
        } else if (fieldName == kResumeAfterField) {
            Status status = checkFieldType(el, Object);
            if (!status.isOK()) {
                return status;
            }
            qr->_resumeAfter = el.embeddedObject();
        } else if (fieldName == kRequestResumeTokenField) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }
            qr->_requestResumeToken = el.boolean();
        } else if (fieldName == kUse44SortKeys) {
            Status status = checkFieldType(el, Bool);
            if (!status.isOK()) {
                return status;
            }
        } else if (isMongocryptdArgument(fieldName)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Failed to parse: " << cmdObj.toString()
                              << ". Unrecognized field '" << fieldName
                              << "'. This command may be meant for a mongocryptd process.");

            // TODO SERVER-47065: A 4.6 node still has to accept the '_use44SortKeys' field, since
            // it could be included in a command sent from a 4.4 mongos. In 4.7 development, this
            // code to tolerate the '_use44SortKeys' field can be deleted.
        } else if (!isGenericArgument(fieldName)) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Failed to parse: " << cmdObj.toString() << ". "
                                        << "Unrecognized field '" << fieldName << "'.");
        }
    }

    auto tailableMode = tailableModeFromBools(tailable, awaitData);
    if (!tailableMode.isOK()) {
        return tailableMode.getStatus();
    }
    qr->_tailableMode = tailableMode.getValue();
    qr->addMetaProjection();

    Status validateStatus = qr->validate();
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    return std::move(qr);
}

StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::makeFromFindCommand(NamespaceString nss,
                                                                            const BSONObj& cmdObj,
                                                                            bool isExplain) {
    BSONElement first = cmdObj.firstElement();
    if (first.type() == BinData && first.binDataType() == BinDataType::newUUID) {
        auto uuid = uassertStatusOK(UUID::parse(first));
        auto qr = std::make_unique<QueryRequest>(NamespaceStringOrUUID(nss.db().toString(), uuid));
        return parseFromFindCommand(std::move(qr), cmdObj, isExplain);
    } else {
        auto qr = std::make_unique<QueryRequest>(nss);
        return parseFromFindCommand(std::move(qr), cmdObj, isExplain);
    }
}

BSONObj QueryRequest::asFindCommand() const {
    BSONObjBuilder bob;
    asFindCommand(&bob);
    return bob.obj();
}

BSONObj QueryRequest::asFindCommandWithUuid() const {
    BSONObjBuilder bob;
    asFindCommandWithUuid(&bob);
    return bob.obj();
}

void QueryRequest::asFindCommand(BSONObjBuilder* cmdBuilder) const {
    cmdBuilder->append(kFindCommandName, _nss.coll());
    asFindCommandInternal(cmdBuilder);
}

void QueryRequest::asFindCommandWithUuid(BSONObjBuilder* cmdBuilder) const {
    invariant(_uuid);
    _uuid->appendToBuilder(cmdBuilder, kFindCommandName);
    asFindCommandInternal(cmdBuilder);
}

void QueryRequest::asFindCommandInternal(BSONObjBuilder* cmdBuilder) const {
    if (!_filter.isEmpty()) {
        cmdBuilder->append(kFilterField, _filter);
    }

    if (!_proj.isEmpty()) {
        cmdBuilder->append(kProjectionField, _proj);
    }

    if (!_sort.isEmpty()) {
        cmdBuilder->append(kSortField, _sort);
    }

    if (!_hint.isEmpty()) {
        cmdBuilder->append(kHintField, _hint);
    }

    if (_readConcern) {
        cmdBuilder->append(repl::ReadConcernArgs::kReadConcernFieldName, *_readConcern);
    }

    if (!_collation.isEmpty()) {
        cmdBuilder->append(kCollationField, _collation);
    }

    if (_skip) {
        cmdBuilder->append(kSkipField, *_skip);
    }

    if (_ntoreturn) {
        cmdBuilder->append(kNToReturnField, *_ntoreturn);
    }

    if (_limit) {
        cmdBuilder->append(kLimitField, *_limit);
    }

    if (_allowDiskUse) {
        cmdBuilder->append(kAllowDiskUseField, true);
    }

    if (_batchSize) {
        cmdBuilder->append(kBatchSizeField, *_batchSize);
    }

    if (!_wantMore) {
        cmdBuilder->append(kSingleBatchField, true);
    }

    if (_maxTimeMS > 0) {
        cmdBuilder->append(cmdOptionMaxTimeMS, _maxTimeMS);
    }

    if (!_max.isEmpty()) {
        cmdBuilder->append(kMaxField, _max);
    }

    if (!_min.isEmpty()) {
        cmdBuilder->append(kMinField, _min);
    }

    if (_returnKey) {
        cmdBuilder->append(kReturnKeyField, true);
    }

    if (_showRecordId) {
        cmdBuilder->append(kShowRecordIdField, true);
    }

    switch (_tailableMode) {
        case TailableModeEnum::kTailable: {
            cmdBuilder->append(kTailableField, true);
            break;
        }
        case TailableModeEnum::kTailableAndAwaitData: {
            cmdBuilder->append(kTailableField, true);
            cmdBuilder->append(kAwaitDataField, true);
            break;
        }
        case TailableModeEnum::kNormal: {
            break;
        }
    }

    if (_noCursorTimeout) {
        cmdBuilder->append(kNoCursorTimeoutField, true);
    }

    if (_allowPartialResults) {
        cmdBuilder->append(kPartialResultsField, true);
    }

    if (_runtimeConstants) {
        BSONObjBuilder rtcBuilder(cmdBuilder->subobjStart(kRuntimeConstantsField));
        _runtimeConstants->serialize(&rtcBuilder);
        rtcBuilder.doneFast();
    }

    if (_letParameters) {
        cmdBuilder->append(kLetField, *_letParameters);
    }

    if (_replicationTerm) {
        cmdBuilder->append(kTermField, *_replicationTerm);
    }

    if (_readOnce) {
        cmdBuilder->append(kReadOnceField, true);
    }

    if (_allowSpeculativeMajorityRead) {
        cmdBuilder->append(kAllowSpeculativeMajorityReadField, true);
    }

    if (_requestResumeToken) {
        cmdBuilder->append(kRequestResumeTokenField, _requestResumeToken);
    }

    if (!_resumeAfter.isEmpty()) {
        cmdBuilder->append(kResumeAfterField, _resumeAfter);
    }
}

void QueryRequest::addShowRecordIdMetaProj() {
    if (_proj["$recordId"]) {
        // There's already some projection on $recordId. Don't overwrite it.
        return;
    }

    BSONObjBuilder projBob;
    projBob.appendElements(_proj);
    BSONObj metaRecordId = BSON("$recordId" << BSON("$meta" << QueryRequest::metaRecordId));
    projBob.append(metaRecordId.firstElement());
    _proj = projBob.obj();
}

Status QueryRequest::validate() const {
    // Min and Max objects must have the same fields.
    if (!_min.isEmpty() && !_max.isEmpty()) {
        if (!_min.isFieldNamePrefixOf(_max) || (_min.nFields() != _max.nFields())) {
            return Status(ErrorCodes::Error(51176), "min and max must have the same field names");
        }
    }

    if ((_limit || _batchSize) && _ntoreturn) {
        return Status(ErrorCodes::BadValue,
                      "'limit' or 'batchSize' fields can not be set with 'ntoreturn' field.");
    }

    if (_skip && *_skip < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Skip value must be non-negative, but received: " << *_skip);
    }

    if (_limit && *_limit < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Limit value must be non-negative, but received: " << *_limit);
    }

    if (_batchSize && *_batchSize < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "BatchSize value must be non-negative, but received: " << *_batchSize);
    }

    if (_ntoreturn && *_ntoreturn < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "NToReturn value must be non-negative, but received: " << *_ntoreturn);
    }

    if (_maxTimeMS < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "MaxTimeMS value must be non-negative, but received: " << _maxTimeMS);
    }

    if (_tailableMode != TailableModeEnum::kNormal) {
        // Tailable cursors cannot have any sort other than {$natural: 1}.
        const BSONObj expectedSort = BSON(kNaturalSortField << 1);
        if (!_sort.isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(_sort != expectedSort)) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with a sort other than {$natural: 1}");
        }

        // Cannot indicate that you want a 'singleBatch' if the cursor is tailable.
        if (!_wantMore) {
            return Status(ErrorCodes::BadValue,
                          "cannot use tailable option with the 'singleBatch' option");
        }
    }

    if (_requestResumeToken) {
        if (SimpleBSONObjComparator::kInstance.evaluate(_hint != BSON(kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "hint must be {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!_sort.isEmpty() &&
            SimpleBSONObjComparator::kInstance.evaluate(_sort != BSON(kNaturalSortField << 1))) {
            return Status(ErrorCodes::BadValue,
                          "sort must be unset or {$natural:1} if 'requestResumeToken' is enabled");
        }
        if (!_resumeAfter.isEmpty()) {
            if (_resumeAfter.nFields() != 1 ||
                _resumeAfter["$recordId"].type() != BSONType::NumberLong) {
                return Status(ErrorCodes::BadValue,
                              "Malformed resume token: the '_resumeAfter' object must contain"
                              " exactly one field named '$recordId', of type NumberLong.");
            }
        }
    } else if (!_resumeAfter.isEmpty()) {
        return Status(ErrorCodes::BadValue,
                      "'requestResumeToken' must be true if 'resumeAfter' is"
                      " specified");
    }
    return Status::OK();
}

// static
StatusWith<int> QueryRequest::parseMaxTimeMS(BSONElement maxTimeMSElt) {
    if (!maxTimeMSElt.eoo() && !maxTimeMSElt.isNumber()) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " must be a number").str());
    }
    long long maxTimeMSLongLong = maxTimeMSElt.safeNumberLong();  // returns 0 on EOO
    if (maxTimeMSLongLong < 0 || maxTimeMSLongLong > INT_MAX) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " is out of range").str());
    }
    double maxTimeMSDouble = maxTimeMSElt.numberDouble();
    if (maxTimeMSElt.type() == mongo::NumberDouble && floor(maxTimeMSDouble) != maxTimeMSDouble) {
        return StatusWith<int>(
            ErrorCodes::BadValue,
            (StringBuilder() << maxTimeMSElt.fieldNameStringData() << " has non-integral value")
                .str());
    }
    return StatusWith<int>(static_cast<int>(maxTimeMSLongLong));
}

bool QueryRequest::isTextScoreMeta(BSONElement elt) {
    // elt must be foo: {$meta: "textScore"}
    if (mongo::Object != elt.type()) {
        return false;
    }
    BSONObj metaObj = elt.Obj();
    BSONObjIterator metaIt(metaObj);
    // must have exactly 1 element
    if (!metaIt.more()) {
        return false;
    }
    BSONElement metaElt = metaIt.next();
    if (metaElt.fieldNameStringData() != "$meta") {
        return false;
    }
    if (mongo::String != metaElt.type()) {
        return false;
    }
    if (StringData{metaElt.valuestr()} != QueryRequest::metaTextScore) {
        return false;
    }
    // must have exactly 1 element
    if (metaIt.more()) {
        return false;
    }
    return true;
}

//
// Old QueryRequest parsing code: SOON TO BE DEPRECATED.
//

// static
StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::fromLegacyQueryMessage(
    const QueryMessage& qm) {
    auto qr = std::make_unique<QueryRequest>(NamespaceString(qm.ns));

    Status status = qr->init(qm.ntoskip, qm.ntoreturn, qm.queryOptions, qm.query, qm.fields, true);
    if (!status.isOK()) {
        return status;
    }

    return std::move(qr);
}

StatusWith<std::unique_ptr<QueryRequest>> QueryRequest::fromLegacyQuery(
    NamespaceStringOrUUID nsOrUuid,
    const BSONObj& queryObj,
    const BSONObj& proj,
    int ntoskip,
    int ntoreturn,
    int queryOptions) {
    auto qr = std::make_unique<QueryRequest>(nsOrUuid);

    Status status = qr->init(ntoskip, ntoreturn, queryOptions, queryObj, proj, true);
    if (!status.isOK()) {
        return status;
    }

    return std::move(qr);
}

Status QueryRequest::init(int ntoskip,
                          int ntoreturn,
                          int queryOptions,
                          const BSONObj& queryObj,
                          const BSONObj& proj,
                          bool fromQueryMessage) {
    _proj = proj.getOwned();

    if (ntoskip) {
        _skip = ntoskip;
    }

    if (ntoreturn) {
        if (ntoreturn < 0) {
            if (ntoreturn == std::numeric_limits<int>::min()) {
                // ntoreturn is negative but can't be negated.
                return Status(ErrorCodes::BadValue, "bad ntoreturn value in query");
            }
            _ntoreturn = -ntoreturn;
            _wantMore = false;
        } else {
            _ntoreturn = ntoreturn;
        }
    }

    // An ntoreturn of 1 is special because it also means to return at most one batch.
    if (_ntoreturn.value_or(0) == 1) {
        _wantMore = false;
    }

    // Initialize flags passed as 'queryOptions' bit vector.
    initFromInt(queryOptions);

    if (fromQueryMessage) {
        BSONElement queryField = queryObj["query"];
        if (!queryField.isABSONObj()) {
            queryField = queryObj["$query"];
        }
        if (queryField.isABSONObj()) {
            _filter = queryField.embeddedObject().getOwned();
            Status status = initFullQuery(queryObj);
            if (!status.isOK()) {
                return status;
            }
        } else {
            _filter = queryObj.getOwned();
        }
        // It's not possible to specify readConcern in a legacy query message, so initialize it to
        // an empty readConcern object, ie. equivalent to `readConcern: {}`.  This ensures that
        // mongos passes this empty readConcern to shards.
        _readConcern = BSONObj();
    } else {
        // This is the debugging code path.
        _filter = queryObj.getOwned();
    }

    _hasReadPref = queryObj.hasField("$readPreference");

    return validate();
}

Status QueryRequest::initFullQuery(const BSONObj& top) {
    BSONObjIterator i(top);

    while (i.more()) {
        BSONElement e = i.next();
        StringData name = e.fieldNameStringData();

        if (name == "$orderby" || name == "orderby") {
            if (Object == e.type()) {
                _sort = e.embeddedObject().getOwned();
            } else if (Array == e.type()) {
                _sort = e.embeddedObject();

                // TODO: Is this ever used?  I don't think so.
                // Quote:
                // This is for languages whose "objects" are not well ordered (JSON is well
                // ordered).
                // [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
                // note: this is slow, but that is ok as order will have very few pieces
                BSONObjBuilder b;
                char p[2] = "0";

                while (1) {
                    BSONObj j = _sort.getObjectField(p);
                    if (j.isEmpty()) {
                        break;
                    }
                    BSONElement e = j.firstElement();
                    if (e.eoo()) {
                        return Status(ErrorCodes::BadValue, "bad order array");
                    }
                    if (!e.isNumber()) {
                        return Status(ErrorCodes::BadValue, "bad order array [2]");
                    }
                    b.append(e);
                    (*p)++;
                    if (!(*p <= '9')) {
                        return Status(ErrorCodes::BadValue, "too many ordering elements");
                    }
                }

                _sort = b.obj();
            } else {
                return Status(ErrorCodes::BadValue, "sort must be object or array");
            }
        } else if (name.startsWith("$")) {
            name = name.substr(1);  // chop first char
            if (name == "explain") {
                // Won't throw.
                _explain = e.trueValue();
            } else if (name == "min") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$min must be a BSONObj");
                }
                _min = e.embeddedObject().getOwned();
            } else if (name == "max") {
                if (!e.isABSONObj()) {
                    return Status(ErrorCodes::BadValue, "$max must be a BSONObj");
                }
                _max = e.embeddedObject().getOwned();
            } else if (name == "hint") {
                if (e.isABSONObj()) {
                    _hint = e.embeddedObject().getOwned();
                } else if (String == e.type()) {
                    _hint = e.wrap();
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "$hint must be either a string or nested object");
                }
            } else if (name == "returnKey") {
                // Won't throw.
                if (e.trueValue()) {
                    _returnKey = true;
                }
            } else if (name == "showDiskLoc") {
                // Won't throw.
                if (e.trueValue()) {
                    _showRecordId = true;
                    addShowRecordIdMetaProj();
                }
            } else if (name == "maxTimeMS") {
                StatusWith<int> maxTimeMS = parseMaxTimeMS(e);
                if (!maxTimeMS.isOK()) {
                    return maxTimeMS.getStatus();
                }
                _maxTimeMS = maxTimeMS.getValue();
            }
        }
    }

    return Status::OK();
}

int QueryRequest::getOptions() const {
    int options = 0;
    if (_tailableMode == TailableModeEnum::kTailable) {
        options |= QueryOption_CursorTailable;
    } else if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
        options |= QueryOption_CursorTailable;
        options |= QueryOption_AwaitData;
    }
    if (_slaveOk) {
        options |= QueryOption_SlaveOk;
    }
    if (_noCursorTimeout) {
        options |= QueryOption_NoCursorTimeout;
    }
    if (_exhaust) {
        options |= QueryOption_Exhaust;
    }
    if (_allowPartialResults) {
        options |= QueryOption_PartialResults;
    }
    return options;
}

void QueryRequest::initFromInt(int options) {
    bool tailable = (options & QueryOption_CursorTailable) != 0;
    bool awaitData = (options & QueryOption_AwaitData) != 0;
    _tailableMode = uassertStatusOK(tailableModeFromBools(tailable, awaitData));
    _slaveOk = (options & QueryOption_SlaveOk) != 0;
    _noCursorTimeout = (options & QueryOption_NoCursorTimeout) != 0;
    _exhaust = (options & QueryOption_Exhaust) != 0;
    _allowPartialResults = (options & QueryOption_PartialResults) != 0;
}

void QueryRequest::addMetaProjection() {
    if (showRecordId()) {
        addShowRecordIdMetaProj();
    }
}

boost::optional<long long> QueryRequest::getEffectiveBatchSize() const {
    return _batchSize ? _batchSize : _ntoreturn;
}

StatusWith<BSONObj> QueryRequest::asAggregationCommand() const {
    BSONObjBuilder aggregationBuilder;

    // First, check if this query has options that are not supported in aggregation.
    if (!_min.isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kMinField << " not supported in aggregation."};
    }
    if (!_max.isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kMaxField << " not supported in aggregation."};
    }
    if (_returnKey) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kReturnKeyField << " not supported in aggregation."};
    }
    if (_showRecordId) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kShowRecordIdField
                              << " not supported in aggregation."};
    }
    if (isTailable()) {
        return {ErrorCodes::InvalidPipelineOperator,
                "Tailable cursors are not supported in aggregation."};
    }
    if (_noCursorTimeout) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kNoCursorTimeoutField
                              << " not supported in aggregation."};
    }
    if (_allowPartialResults) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kPartialResultsField
                              << " not supported in aggregation."};
    }
    if (_ntoreturn) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot convert to an aggregation if ntoreturn is set."};
    }
    if (_sort[kNaturalSortField]) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Sort option " << kNaturalSortField
                              << " not supported in aggregation."};
    }
    // The aggregation command normally does not support the 'singleBatch' option, but we make a
    // special exception if 'limit' is set to 1.
    if (!_wantMore && _limit.value_or(0) != 1LL) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kSingleBatchField
                              << " not supported in aggregation."};
    }
    if (_readOnce) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kReadOnceField << " not supported in aggregation."};
    }

    if (_allowSpeculativeMajorityRead) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kAllowSpeculativeMajorityReadField
                              << " not supported in aggregation."};
    }

    if (_requestResumeToken) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kRequestResumeTokenField
                              << " not supported in aggregation."};
    }

    if (!_resumeAfter.isEmpty()) {
        return {ErrorCodes::InvalidPipelineOperator,
                str::stream() << "Option " << kResumeAfterField
                              << " not supported in aggregation."};
    }

    // Now that we've successfully validated this QR, begin building the aggregation command.
    aggregationBuilder.append("aggregate", _nss.coll());

    // Construct an aggregation pipeline that finds the equivalent documents to this query request.
    BSONArrayBuilder pipelineBuilder(aggregationBuilder.subarrayStart("pipeline"));
    if (!_filter.isEmpty()) {
        BSONObjBuilder matchBuilder(pipelineBuilder.subobjStart());
        matchBuilder.append("$match", _filter);
        matchBuilder.doneFast();
    }
    if (!_sort.isEmpty()) {
        BSONObjBuilder sortBuilder(pipelineBuilder.subobjStart());
        sortBuilder.append("$sort", _sort);
        sortBuilder.doneFast();
    }
    if (_skip) {
        BSONObjBuilder skipBuilder(pipelineBuilder.subobjStart());
        skipBuilder.append("$skip", *_skip);
        skipBuilder.doneFast();
    }
    if (_limit) {
        BSONObjBuilder limitBuilder(pipelineBuilder.subobjStart());
        limitBuilder.append("$limit", *_limit);
        limitBuilder.doneFast();
    }
    if (!_proj.isEmpty()) {
        BSONObjBuilder projectBuilder(pipelineBuilder.subobjStart());
        projectBuilder.append("$project", _proj);
        projectBuilder.doneFast();
    }
    pipelineBuilder.doneFast();

    // The aggregation 'cursor' option is always set, regardless of the presence of batchSize.
    BSONObjBuilder batchSizeBuilder(aggregationBuilder.subobjStart("cursor"));
    if (_batchSize) {
        batchSizeBuilder.append(kBatchSizeField, *_batchSize);
    }
    batchSizeBuilder.doneFast();

    // Other options.
    aggregationBuilder.append("collation", _collation);
    if (_maxTimeMS > 0) {
        aggregationBuilder.append(cmdOptionMaxTimeMS, _maxTimeMS);
    }
    if (!_hint.isEmpty()) {
        aggregationBuilder.append("hint", _hint);
    }
    if (_readConcern) {
        aggregationBuilder.append("readConcern", *_readConcern);
    }
    if (!_unwrappedReadPref.isEmpty()) {
        aggregationBuilder.append(QueryRequest::kUnwrappedReadPrefField, _unwrappedReadPref);
    }
    if (_allowDiskUse) {
        aggregationBuilder.append(QueryRequest::kAllowDiskUseField, _allowDiskUse);
    }
    if (_runtimeConstants) {
        BSONObjBuilder rtcBuilder(aggregationBuilder.subobjStart(kRuntimeConstantsField));
        _runtimeConstants->serialize(&rtcBuilder);
        rtcBuilder.doneFast();
    }
    if (_letParameters) {
        aggregationBuilder.append(QueryRequest::kLetField, *_letParameters);
    }
    return StatusWith<BSONObj>(aggregationBuilder.obj());
}
}  // namespace mongo

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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {
class Timestamp;
class RecordId;
namespace record_id_helpers {

/**
 * Converts Timestamp to a RecordId in an unspecified manor that is safe to use as the key to
 * in a RecordStore.
 */
StatusWith<RecordId> keyForOptime(const Timestamp& opTime);

/**
 * For collections that use clustering by _id, converts various values into a RecordId.
 */
StatusWith<RecordId> keyForDoc(const BSONObj& doc);
RecordId keyForElem(const BSONElement& elem);
RecordId keyForOID(OID oid);

/**
 * data and len must be the arguments from RecordStore::insert() on an oplog collection.
 */
StatusWith<RecordId> extractKeyOptime(const char* data, int len);

/**
 * Helpers to append RecordIds to a BSON object builder. Note that this resolves the underlying BSON
 * type of the RecordId if it stores a KeyString.
 *
 * This should be used for informational purposes only. This cannot be 'round-tripped' back into a
 * RecordId because it loses information about the original RecordId format. If you require passing
 * a RecordId as a token or storing for a resumable scan, for example, use RecordId::serializeToken.
 */
void appendToBSONAs(RecordId rid, BSONObjBuilder* builder, StringData fieldName);
BSONObj toBSONAs(RecordId rid, StringData fieldName);

}  // namespace record_id_helpers
}  // namespace mongo

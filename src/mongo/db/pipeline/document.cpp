
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

#include "mongo/db/pipeline/document.h"

#include <boost/functional/hash.hpp>

#include "mongo/bson/bson_depth.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
using namespace mongoutils;
using boost::intrusive_ptr;
using std::string;
using std::vector;

const DocumentStorage DocumentStorage::kEmptyDoc;

const std::vector<StringData> Document::allMetadataFieldNames = {Document::metaFieldTextScore,
                                                                 Document::metaFieldRandVal,
                                                                 Document::metaFieldSortKey,
                                                                 Document::metaFieldGeoNearDistance,
                                                                 Document::metaFieldGeoNearPoint};

Position DocumentStorage::findField(StringData requested) const {
    int reqSize = requested.size();  // get size calculation out of the way if needed

    if (_numFields >= HASH_TAB_MIN) {  // hash lookup
        const unsigned bucket = bucketForKey(requested);

        Position pos = _hashTab[bucket];
        while (pos.found()) {
            const ValueElement& elem = getField(pos);
            if (elem.nameLen == reqSize && memcmp(requested.rawData(), elem._name, reqSize) == 0) {
                return pos;
            }

            // possible collision
            pos = elem.nextCollision;
        }
    } else {  // linear scan
        for (DocumentStorageIterator it = iteratorAll(); !it.atEnd(); it.advance()) {
            if (it->nameLen == reqSize && memcmp(requested.rawData(), it->_name, reqSize) == 0) {
                return it.position();
            }
        }
    }

    // if we got here, there's no such field
    return Position();
}

Value& DocumentStorage::appendField(StringData name) {
    Position pos = getNextPosition();
    const int nameSize = name.size();

    // these are the same for everyone
    const Position nextCollision;
    const Value value;

    // Make room for new field (and padding at end for alignment)
    const unsigned newUsed = ValueElement::align(_usedBytes + sizeof(ValueElement) + nameSize);
    if (_buffer + newUsed > _bufferEnd)
        alloc(newUsed);
    _usedBytes = newUsed;

    // Append structure of a ValueElement
    char* dest = _buffer + pos.index;  // must be after alloc since it changes _buffer
#define append(x)                  \
    memcpy(dest, &(x), sizeof(x)); \
    dest += sizeof(x)
    append(value);
    append(nextCollision);
    append(nameSize);
    name.copyTo(dest, true);
// Padding for alignment handled above
#undef append

    // Make sure next field starts where we expect it
    fassert(16486, getField(pos).next()->ptr() == _buffer + _usedBytes);

    _numFields++;

    if (_numFields > HASH_TAB_MIN) {
        addFieldToHashTable(pos);
    } else if (_numFields == HASH_TAB_MIN) {
        // adds all fields to hash table (including the one we just added)
        rehash();
    }

    return getField(pos).val;
}

// Call after adding field to _fields and increasing _numFields
void DocumentStorage::addFieldToHashTable(Position pos) {
    ValueElement& elem = getField(pos);
    elem.nextCollision = Position();

    const unsigned bucket = bucketForKey(elem.nameSD());

    Position* posPtr = &_hashTab[bucket];
    while (posPtr->found()) {
        // collision: walk links and add new to end
        posPtr = &getField(*posPtr).nextCollision;
    }
    *posPtr = Position(pos.index);
}

void DocumentStorage::alloc(unsigned newSize) {
    const bool firstAlloc = !_buffer;
    const bool doingRehash = needRehash();
    const size_t oldCapacity = _bufferEnd - _buffer;

    // make new bucket count big enough
    while (needRehash() || hashTabBuckets() < HASH_TAB_INIT_SIZE)
        _hashTabMask = hashTabBuckets() * 2 - 1;

    // only allocate power-of-two sized space > 128 bytes
    size_t capacity = 128;
    while (capacity < newSize + hashTabBytes())
        capacity *= 2;

    uassert(16490, "Tried to make oversized document", capacity <= size_t(BufferMaxSize));

    std::unique_ptr<char[]> oldBuf(_buffer);
    _buffer = new char[capacity];
    _bufferEnd = _buffer + capacity - hashTabBytes();

    if (!firstAlloc) {
        // This just copies the elements
        memcpy(_buffer, oldBuf.get(), _usedBytes);

        if (_numFields >= HASH_TAB_MIN) {
            // if we were hashing, deal with the hash table
            if (doingRehash) {
                rehash();
            } else {
                // no rehash needed so just slide table down to new position
                memcpy(_hashTab, oldBuf.get() + oldCapacity, hashTabBytes());
            }
        }
    }
}

void DocumentStorage::reserveFields(size_t expectedFields) {
    fassert(16487, !_buffer);

    unsigned buckets = HASH_TAB_INIT_SIZE;
    while (buckets < expectedFields)
        buckets *= 2;
    _hashTabMask = buckets - 1;

    // Using expectedFields+1 to allow space for long field names
    const size_t newSize = (expectedFields + 1) * ValueElement::align(sizeof(ValueElement));

    uassert(16491, "Tried to make oversized document", newSize <= size_t(BufferMaxSize));

    _buffer = new char[newSize + hashTabBytes()];
    _bufferEnd = _buffer + newSize;
}

intrusive_ptr<DocumentStorage> DocumentStorage::clone() const {
    auto out = make_intrusive<DocumentStorage>();

    // Make a copy of the buffer.
    // It is very important that the positions of each field are the same after cloning.
    const size_t bufferBytes = allocatedBytes();
    out->_buffer = new char[bufferBytes];
    out->_bufferEnd = out->_buffer + (_bufferEnd - _buffer);
    if (bufferBytes > 0) {
        memcpy(out->_buffer, _buffer, bufferBytes);
    }

    // Copy remaining fields
    out->_usedBytes = _usedBytes;
    out->_numFields = _numFields;
    out->_hashTabMask = _hashTabMask;
    out->_metaFields = _metaFields;
    out->_textScore = _textScore;
    out->_randVal = _randVal;
    out->_sortKey = _sortKey.getOwned();
    out->_geoNearDistance = _geoNearDistance;
    out->_geoNearPoint = _geoNearPoint.getOwned();

    // Tell values that they have been memcpyed (updates ref counts)
    for (DocumentStorageIterator it = out->iteratorAll(); !it.atEnd(); it.advance()) {
        it->val.memcpyed();
    }

    return out;
}

DocumentStorage::~DocumentStorage() {
    std::unique_ptr<char[]> deleteBufferAtScopeEnd(_buffer);

    for (DocumentStorageIterator it = iteratorAll(); !it.atEnd(); it.advance()) {
        it->val.~Value();  // explicit destructor call
    }
}

Document::Document(const BSONObj& bson) {
    MutableDocument md(bson.nFields());

    BSONObjIterator it(bson);
    while (it.more()) {
        BSONElement bsonElement(it.next());
        md.addField(bsonElement.fieldNameStringData(), Value(bsonElement));
    }

    *this = md.freeze();
}

Document::Document(std::initializer_list<std::pair<StringData, ImplicitValue>> initializerList) {
    MutableDocument mutableDoc(initializerList.size());

    for (auto&& pair : initializerList) {
        mutableDoc.addField(pair.first, pair.second);
    }

    *this = mutableDoc.freeze();
}

BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const Document& doc) {
    BSONObjBuilder subobj(builder.subobjStart());
    doc.toBson(&subobj);
    subobj.doneFast();
    return builder.builder();
}

void Document::toBson(BSONObjBuilder* builder, size_t recursionLevel) const {
    uassert(ErrorCodes::Overflow,
            str::stream() << "cannot convert document to BSON because it exceeds the limit of "
                          << BSONDepth::getMaxAllowableDepth()
                          << " levels of nesting",
            recursionLevel <= BSONDepth::getMaxAllowableDepth());

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        it->val.addToBsonObj(builder, it->nameSD(), recursionLevel);
    }
}

BSONObj Document::toBson() const {
    BSONObjBuilder bb;
    toBson(&bb);
    return bb.obj();
}

constexpr StringData Document::metaFieldTextScore;
constexpr StringData Document::metaFieldRandVal;
constexpr StringData Document::metaFieldSortKey;
constexpr StringData Document::metaFieldGeoNearDistance;
constexpr StringData Document::metaFieldGeoNearPoint;

BSONObj Document::toBsonWithMetaData() const {
    BSONObjBuilder bb;
    toBson(&bb);
    if (hasTextScore())
        bb.append(metaFieldTextScore, getTextScore());
    if (hasRandMetaField())
        bb.append(metaFieldRandVal, getRandMetaField());
    if (hasSortKeyMetaField())
        bb.append(metaFieldSortKey, getSortKeyMetaField());
    if (hasGeoNearDistance())
        bb.append(metaFieldGeoNearDistance, getGeoNearDistance());
    if (hasGeoNearPoint())
        getGeoNearPoint().addToBsonObj(&bb, metaFieldGeoNearPoint);
    return bb.obj();
}

Document Document::fromBsonWithMetaData(const BSONObj& bson) {
    MutableDocument md;

    BSONObjIterator it(bson);
    while (it.more()) {
        BSONElement elem(it.next());
        auto fieldName = elem.fieldNameStringData();
        if (fieldName[0] == '$') {
            if (fieldName == metaFieldTextScore) {
                md.setTextScore(elem.Double());
                continue;
            } else if (fieldName == metaFieldRandVal) {
                md.setRandMetaField(elem.Double());
                continue;
            } else if (fieldName == metaFieldSortKey) {
                md.setSortKeyMetaField(elem.Obj());
                continue;
            } else if (fieldName == metaFieldGeoNearDistance) {
                md.setGeoNearDistance(elem.Double());
                continue;
            } else if (fieldName == metaFieldGeoNearPoint) {
                Value val;
                if (elem.type() == BSONType::Array) {
                    val = Value(BSONArray(elem.embeddedObject()));
                } else {
                    invariant(elem.type() == BSONType::Object);
                    val = Value(elem.embeddedObject());
                }

                md.setGeoNearPoint(val);
                continue;
            }
        }

        // Note: this will not parse out metadata in embedded documents.
        md.addField(fieldName, Value(elem));
    }

    return md.freeze();
}

MutableDocument::MutableDocument(size_t expectedFields)
    : _storageHolder(NULL), _storage(_storageHolder) {
    if (expectedFields) {
        storage().reserveFields(expectedFields);
    }
}

MutableValue MutableDocument::getNestedFieldHelper(const FieldPath& dottedField, size_t level) {
    if (level == dottedField.getPathLength() - 1) {
        return getField(dottedField.getFieldName(level));
    } else {
        MutableDocument nested(getField(dottedField.getFieldName(level)));
        return nested.getNestedFieldHelper(dottedField, level + 1);
    }
}

MutableValue MutableDocument::getNestedField(const FieldPath& dottedField) {
    fassert(16601, dottedField.getPathLength());
    return getNestedFieldHelper(dottedField, 0);
}

MutableValue MutableDocument::getNestedFieldHelper(const vector<Position>& positions,
                                                   size_t level) {
    if (level == positions.size() - 1) {
        return getField(positions[level]);
    } else {
        MutableDocument nested(getField(positions[level]));
        return nested.getNestedFieldHelper(positions, level + 1);
    }
}

MutableValue MutableDocument::getNestedField(const vector<Position>& positions) {
    fassert(16488, !positions.empty());
    return getNestedFieldHelper(positions, 0);
}

static Value getNestedFieldHelper(const Document& doc,
                                  const FieldPath& fieldNames,
                                  vector<Position>* positions,
                                  size_t level) {
    const auto fieldName = fieldNames.getFieldName(level);
    const Position pos = doc.positionOf(fieldName);

    if (!pos.found())
        return Value();

    if (positions)
        positions->push_back(pos);

    if (level == fieldNames.getPathLength() - 1)
        return doc.getField(pos);

    Value val = doc.getField(pos);
    if (val.getType() != Object)
        return Value();

    return getNestedFieldHelper(val.getDocument(), fieldNames, positions, level + 1);
}

const Value Document::getNestedField(const FieldPath& path, vector<Position>* positions) const {
    fassert(16489, path.getPathLength());
    return getNestedFieldHelper(*this, path, positions, 0);
}

size_t Document::getApproximateSize() const {
    if (!_storage)
        return 0;  // we've allocated no memory

    size_t size = sizeof(DocumentStorage);
    size += storage().allocatedBytes();

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        size += it->val.getApproximateSize();
        size -= sizeof(Value);  // already accounted for above
    }

    return size;
}

void Document::hash_combine(size_t& seed,
                            const StringData::ComparatorInterface* stringComparator) const {
    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        StringData name = it->nameSD();
        boost::hash_range(seed, name.rawData(), name.rawData() + name.size());
        it->val.hash_combine(seed, stringComparator);
    }
}

int Document::compare(const Document& rL,
                      const Document& rR,
                      const StringData::ComparatorInterface* stringComparator) {
    DocumentStorageIterator lIt = rL.storage().iterator();
    DocumentStorageIterator rIt = rR.storage().iterator();

    while (true) {
        if (lIt.atEnd()) {
            if (rIt.atEnd())
                return 0;  // documents are the same length

            return -1;  // left document is shorter
        }

        if (rIt.atEnd())
            return 1;  // right document is shorter

        const ValueElement& rField = rIt.get();
        const ValueElement& lField = lIt.get();

        // For compatibility with BSONObj::woCompare() consider the canonical type of values
        // before considerting their names.
        if (lField.val.getType() != rField.val.getType()) {
            const int rCType = canonicalizeBSONType(rField.val.getType());
            const int lCType = canonicalizeBSONType(lField.val.getType());
            if (lCType != rCType)
                return lCType < rCType ? -1 : 1;
        }

        const int nameCmp = lField.nameSD().compare(rField.nameSD());
        if (nameCmp)
            return nameCmp;  // field names are unequal

        const int valueCmp = Value::compare(lField.val, rField.val, stringComparator);
        if (valueCmp)
            return valueCmp;  // fields are unequal

        rIt.advance();
        lIt.advance();
    }
}

string Document::toString() const {
    if (empty())
        return "{}";

    StringBuilder out;
    const char* prefix = "{";

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        out << prefix << it->nameSD() << ": " << it->val.toString();
        prefix = ", ";
    }
    out << '}';

    return out.str();
}

void Document::serializeForSorter(BufBuilder& buf) const {
    const int numElems = size();
    buf.appendNum(numElems);

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        buf.appendStr(it->nameSD(), /*NUL byte*/ true);
        it->val.serializeForSorter(buf);
    }

    if (hasTextScore()) {
        buf.appendNum(char(DocumentStorage::MetaType::TEXT_SCORE + 1));
        buf.appendNum(getTextScore());
    }
    if (hasRandMetaField()) {
        buf.appendNum(char(DocumentStorage::MetaType::RAND_VAL + 1));
        buf.appendNum(getRandMetaField());
    }
    if (hasSortKeyMetaField()) {
        buf.appendNum(char(DocumentStorage::MetaType::SORT_KEY + 1));
        getSortKeyMetaField().appendSelfToBufBuilder(buf);
    }
    buf.appendNum(char(0));
}

Document Document::deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
    const int numElems = buf.read<LittleEndian<int>>();
    MutableDocument doc(numElems);
    for (int i = 0; i < numElems; i++) {
        StringData name = buf.readCStr();
        doc.addField(name, Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
    }

    while (char marker = buf.read<char>()) {
        if (marker == char(DocumentStorage::MetaType::TEXT_SCORE) + 1) {
            doc.setTextScore(buf.read<LittleEndian<double>>());
        } else if (marker == char(DocumentStorage::MetaType::RAND_VAL) + 1) {
            doc.setRandMetaField(buf.read<LittleEndian<double>>());
        } else if (marker == char(DocumentStorage::MetaType::SORT_KEY) + 1) {
            doc.setSortKeyMetaField(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else {
            uasserted(28744, "Unrecognized marker, unable to deserialize buffer");
        }
    }

    return doc.freeze();
}
}

/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/logv2/json_formatter.h"

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/logv2/name_extractor.h"
#include "mongo/util/str_escape.h"
#include "mongo/util/time_support.h"

#include <fmt/format.h>

namespace mongo::logv2 {
namespace {
struct JSONValueExtractor {
    JSONValueExtractor(fmt::memory_buffer& buffer, size_t attributeMaxSize)
        : _buffer(buffer), _attributeMaxSize(attributeMaxSize) {}

    void operator()(StringData name, CustomAttributeValue const& val) {
        // Try to format as BSON first if available. Prefer BSONAppend if available as we might only
        // want the value and not the whole element.
        if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            BSONElement element = builder.done().getField(name);
            BSONObj truncated = element.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                         false,
                                                         false,
                                                         0,
                                                         _buffer,
                                                         _attributeMaxSize);
            addTruncationReport(name, truncated, element.size());
        } else if (val.BSONSerialize) {
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            BSONObj obj = builder.done();
            BSONObj truncated = obj.jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, _buffer, _attributeMaxSize);
            addTruncationReport(name, truncated, builder.done().objsize());

        } else if (val.toBSONArray) {
            // This is a JSON subarray, no quotes needed
            storeUnquoted(name);
            BSONArray arr = val.toBSONArray();
            BSONObj truncated = arr.jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, _buffer, _attributeMaxSize);
            addTruncationReport(name, truncated, arr.objsize());

        } else if (val.stringSerialize) {
            fmt::memory_buffer intermediate;
            val.stringSerialize(intermediate);
            storeQuoted(name, StringData(intermediate.data(), intermediate.size()));
        } else {
            // This is a string, surround value with quotes
            storeQuoted(name, val.toString());
        }
    }

    void operator()(StringData name, const BSONObj* val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val->jsonStringBuffer(
            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, _buffer, _attributeMaxSize);
        addTruncationReport(name, truncated, val->objsize());
    }

    void operator()(StringData name, const BSONArray* val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val->jsonStringBuffer(
            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, _buffer, _attributeMaxSize);
        addTruncationReport(name, truncated, val->objsize());
    }

    void operator()(StringData name, StringData value) {
        storeQuoted(name, value);
    }

    template <typename Period>
    void operator()(StringData name, const Duration<Period>& value) {
        fmt::format_to(
            _buffer, R"({}"{}{}":{})", _separator, name, value.mongoUnitSuffix(), value.count());
        _separator = ","_sd;
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
        storeUnquotedValue(name, value);
    }

    BSONObj truncated() {
        return _truncated.done();
    }

    BSONObj truncatedSizes() {
        return _truncatedSizes.done();
    }

private:
    void storeUnquoted(StringData name) {
        fmt::format_to(_buffer, R"({}"{}":)", _separator, name);
        _separator = ","_sd;
    }

    template <typename T>
    void storeUnquotedValue(StringData name, const T& value) {
        fmt::format_to(_buffer, R"({}"{}":{})", _separator, name, value);
        _separator = ","_sd;
    }

    template <typename T>
    void storeQuoted(StringData name, const T& value) {
        fmt::format_to(_buffer, R"({}"{}":")", _separator, name);
        std::size_t before = _buffer.size();
        str::escapeForJSON(_buffer, value);
        if (_attributeMaxSize != 0) {
            auto truncatedEnd =
                str::UTF8SafeTruncation(_buffer.begin() + before, _buffer.end(), _attributeMaxSize);
            if (truncatedEnd != _buffer.end()) {
                BSONObjBuilder truncationInfo = _truncated.subobjStart(name);
                truncationInfo.append("type"_sd, typeName(BSONType::String));
                truncationInfo.append("size"_sd, static_cast<int64_t>(_buffer.size() - before));
                truncationInfo.done();
            }

            _buffer.resize(truncatedEnd - _buffer.begin());
        }

        _buffer.push_back('"');
        _separator = ","_sd;
    }

    void addTruncationReport(StringData name, const BSONObj& truncated, int64_t objsize) {
        if (!truncated.isEmpty()) {
            _truncated.append(name, truncated);
            _truncatedSizes.append(name, objsize);
        }
    }

    fmt::memory_buffer& _buffer;
    BSONObjBuilder _truncated;
    BSONObjBuilder _truncatedSizes;
    StringData _separator = ""_sd;
    size_t _attributeMaxSize;
};
}  // namespace

void JSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using namespace boost::log;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    StringData severity =
        extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact();
    StringData component =
        extract<LogComponent>(attributes::component(), rec).get().getNameForLog();
    std::string tag;
    LogTag tags = extract<LogTag>(attributes::tags(), rec).get();
    if (tags != LogTag::kNone) {
        tag = fmt::format(
            ",\"{}\":{}",
            constants::kTagsFieldName,
            tags.toBSONArray().jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    }

    fmt::memory_buffer buffer;

    // Put all fields up until the message value
    fmt::format_to(buffer,
                   R"({{)"
                   R"("{}":{{"$date":")",
                   constants::kTimestampFieldName);
    Date_t date = extract<Date_t>(attributes::timeStamp(), rec).get();
    switch (_timestampFormat) {
        case LogTimestampFormat::kISO8601UTC:
            outputDateAsISOStringUTC(buffer, date);
            break;
        case LogTimestampFormat::kISO8601Local:
            outputDateAsISOStringLocal(buffer, date);
            break;
    };
    fmt::format_to(buffer,
                   R"("}},)"              // close timestamp
                   R"("{}":"{}"{: <{}})"  // severity with padding for the comma
                   R"("{}":"{}"{: <{}})"  // component with padding for the comma
                   R"("{}":{},)"          // id
                   R"("{}":"{}",)"        // context
                   R"("{}":"{}")",        // message
                   // severity, left align the comma and add padding to create fixed column width
                   constants::kSeverityFieldName,
                   severity,
                   ",",
                   3 - severity.size(),
                   // component, left align the comma and add padding to create fixed column width
                   constants::kComponentFieldName,
                   component,
                   ",",
                   9 - component.size(),
                   // id
                   constants::kIdFieldName,
                   extract<int32_t>(attributes::id(), rec).get(),
                   // context
                   constants::kContextFieldName,
                   extract<StringData>(attributes::threadName(), rec).get(),
                   // message
                   constants::kMessageFieldName,
                   extract<StringData>(attributes::message(), rec).get());

    if (!attrs.empty()) {
        fmt::format_to(buffer, R"(,"{}":{{)", constants::kAttributesFieldName);
        // comma separated list of attributes (no opening/closing brace are added here)
        size_t attributeMaxSize = 0;
        if (extract<LogTruncation>(attributes::truncation(), rec).get() == LogTruncation::Enabled) {
            if (_maxAttributeSizeKB)
                attributeMaxSize = _maxAttributeSizeKB->loadRelaxed() * 1024;
            else
                attributeMaxSize = constants::kDefaultMaxAttributeOutputSizeKB * 1024;
        }
        JSONValueExtractor extractor(buffer, attributeMaxSize);
        attrs.apply(extractor);
        buffer.push_back('}');

        if (BSONObj truncated = extractor.truncated(); !truncated.isEmpty()) {
            fmt::format_to(buffer, R"(,"{}":)", constants::kTruncatedFieldName);
            truncated.jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer, 0);
        }

        if (BSONObj truncatedSizes = extractor.truncatedSizes(); !truncatedSizes.isEmpty()) {
            fmt::format_to(buffer, R"(,"{}":)", constants::kTruncatedSizeFieldName);
            truncatedSizes.jsonStringBuffer(
                JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer, 0);
        }
    }

    // Add remaining fields
    fmt::format_to(buffer,
                   R"({})"  // optional tags
                   R"(}})",
                   // tags
                   tag);

    // Write final JSON object to output stream
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2

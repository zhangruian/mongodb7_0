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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/logv2/log_test_v2.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/composite_backend.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/logv2/uassert_sink.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <boost/log/attributes/constant.hpp>
#include <boost/optional.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace mongo {
namespace logv2 {
namespace {

struct TypeWithoutBSON {
    TypeWithoutBSON() {}
    TypeWithoutBSON(double x, double y) : _x(x), _y(y) {}

    double _x{0.0};
    double _y{0.0};

    std::string toString() const {
        return fmt::format("(x: {}, y: {})", _x, _y);
    }
};

struct TypeWithOnlyStringSerialize {
    TypeWithOnlyStringSerialize() {}
    TypeWithOnlyStringSerialize(double x, double y) : _x(x), _y(y) {}

    double _x{0.0};
    double _y{0.0};

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(buffer, "(x: {}, y: {})", _x, _y);
    }
};

struct TypeWithBothStringFormatters {
    TypeWithBothStringFormatters() {}

    std::string toString() const {
        return fmt::format("toString");
    }

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(buffer, "serialize");
    }
};

struct TypeWithBSON : public TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("x"_sd, _x);
        builder.append("y"_sd, _y);
        return builder.obj();
    }
};

struct TypeWithBSONSerialize : public TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    void serialize(BSONObjBuilder* builder) const {
        builder->append("x"_sd, _x);
        builder->append("y"_sd, _y);
        builder->append("type"_sd, "serialize"_sd);
    }
};

struct TypeWithBothBSONFormatters : public TypeWithBSON {
    using TypeWithBSON::TypeWithBSON;

    void serialize(BSONObjBuilder* builder) const {
        builder->append("x"_sd, _x);
        builder->append("y"_sd, _y);
        builder->append("type"_sd, "serialize"_sd);
    }
};

struct TypeWithBSONArray {
    std::string toString() const {
        return "first, second";
    }
    BSONArray toBSONArray() const {
        BSONArrayBuilder builder;
        builder.append("first"_sd);
        builder.append("second"_sd);
        return builder.arr();
    }
};

enum UnscopedEnumWithToString { UnscopedEntryWithToString };

std::string toString(UnscopedEnumWithToString val) {
    return "UnscopedEntryWithToString";
}

struct TypeWithNonMemberFormatting {};

std::string toString(const TypeWithNonMemberFormatting&) {
    return "TypeWithNonMemberFormatting";
}

BSONObj toBSON(const TypeWithNonMemberFormatting&) {
    BSONObjBuilder builder;
    builder.append("first"_sd, "TypeWithNonMemberFormatting");
    return builder.obj();
}

class LogDuringInitShutdownTester {
public:
    LogDuringInitShutdownTester() {

        auto sink = LogCaptureBackend::create(lines);
        sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
        sink->set_formatter(PlainFormatter());
        boost::log::core::get()->add_sink(sink);

        LOGV2(20001, "log during init");
        ASSERT_EQUALS(lines.back(), "log during init");
    }
    ~LogDuringInitShutdownTester() {
        LOGV2(4600800, "log during shutdown");
        ASSERT_EQUALS(lines.back(), "log during shutdown");
    }

    std::vector<std::string> lines;
};

LogDuringInitShutdownTester logDuringInitAndShutdown;

TEST_F(LogTestV2, Basic) {
    std::vector<std::string> lines;
    auto sink = LogCaptureBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    BSONObjBuilder builder;
    fmt::memory_buffer buffer;

    LOGV2(20002, "test");
    ASSERT_EQUALS(lines.back(), "test");

    LOGV2_DEBUG(20063, -2, "test debug");
    ASSERT_EQUALS(lines.back(), "test debug");

    LOGV2(20003, "test {name}", "name"_attr = 1);
    ASSERT_EQUALS(lines.back(), "test 1");

    LOGV2(20004, "test {name:d}", "name"_attr = 2);
    ASSERT_EQUALS(lines.back(), "test 2");

    LOGV2(20005, "test {name}", "name"_attr = "char*");
    ASSERT_EQUALS(lines.back(), "test char*");

    LOGV2(20006, "test {name}", "name"_attr = std::string("std::string"));
    ASSERT_EQUALS(lines.back(), "test std::string");

    LOGV2(20007, "test {name}", "name"_attr = "StringData"_sd);
    ASSERT_EQUALS(lines.back(), "test StringData");

    LOGV2_OPTIONS(20064, {LogTag::kStartupWarnings}, "test");
    ASSERT_EQUALS(lines.back(), "test");

    TypeWithBSON t(1.0, 2.0);
    LOGV2(20008, "{name} custom formatting", "name"_attr = t);
    ASSERT_EQUALS(lines.back(), t.toString() + " custom formatting");

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20009, "{name} custom formatting, no bson", "name"_attr = t2);
    ASSERT_EQUALS(lines.back(), t.toString() + " custom formatting, no bson");

    TypeWithOnlyStringSerialize t3(1.0, 2.0);
    LOGV2(20010, "{name}", "name"_attr = t3);
    buffer.clear();
    t3.serialize(buffer);
    ASSERT_EQUALS(lines.back(), fmt::to_string(buffer));

    // Serialize should be preferred when both are available
    TypeWithBothStringFormatters t4;
    LOGV2(20011, "{name}", "name"_attr = t4);
    buffer.clear();
    t4.serialize(buffer);
    ASSERT_EQUALS(lines.back(), fmt::to_string(buffer));

    // Message string is selected when using API that also take a format string
    LOGV2(20084, "fmtstr {name}", "msgstr", "name"_attr = 1);
    ASSERT_EQUALS(lines.back(), "msgstr");

    // Test that logging exceptions does not propagate out to user code in release builds
    if (!kDebugBuild) {
        LOGV2(4638203, "mismatch {name}", "not_name"_attr = 1);
        ASSERT(StringData(lines.back()).startsWith("Exception during log"_sd));
    }
}

TEST_F(LogTestV2, Types) {
    using namespace constants;

    std::vector<std::string> text;
    auto text_sink = LogCaptureBackend::create(text);
    text_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    text_sink->set_formatter(PlainFormatter());
    attach(text_sink);

    std::vector<std::string> json;
    auto json_sink = LogCaptureBackend::create(json);
    json_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    json_sink->set_formatter(JSONFormatter());
    attach(json_sink);

    std::vector<std::string> bson;
    auto bson_sink = LogCaptureBackend::create(bson);
    bson_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    bson_sink->set_formatter(BSONFormatter());
    attach(bson_sink);

    // The JSON formatter should make the types round-trippable without data loss
    auto validateJSON = [&](auto expected) {
        namespace pt = boost::property_tree;

        std::istringstream json_stream(json.back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        ASSERT_EQUALS(ptree.get<decltype(expected)>(std::string(kAttributesFieldName) + ".name"),
                      expected);
    };

    auto lastBSONElement = [&]() {
        return BSONObj(bson.back().data()).getField(kAttributesFieldName).Obj().getField("name"_sd);
    };

    auto testIntegral = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text.clear();
            LOGV2(20012, "{name}", "name"_attr = value);
            ASSERT_EQUALS(text.back(), fmt::format("{}", value));
            validateJSON(value);

            // TODO: We should have been able to use std::make_signed here but it is broken on
            // Visual Studio 2017 and 2019
            using T = decltype(value);
            if constexpr (std::is_same_v<T, unsigned long long>) {
                ASSERT_EQUALS(lastBSONElement().Number(), static_cast<long long>(value));
            } else if constexpr (std::is_same_v<T, unsigned long>) {
                ASSERT_EQUALS(lastBSONElement().Number(), static_cast<int64_t>(value));
            } else {
                ASSERT_EQUALS(lastBSONElement().Number(), value);
            }
        };

        test(std::numeric_limits<T>::max());
        test(std::numeric_limits<T>::min());
        test(std::numeric_limits<T>::lowest());
        test(static_cast<T>(-10));
        test(static_cast<T>(-2));
        test(static_cast<T>(-1));
        test(static_cast<T>(0));
        test(static_cast<T>(1));
        test(static_cast<T>(2));
        test(static_cast<T>(10));
    };

    auto testFloatingPoint = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text.clear();
            LOGV2(20013, "{name}", "name"_attr = value);
            // Floats are formatted as double
            ASSERT_EQUALS(text.back(), fmt::format("{}", static_cast<double>(value)));
            validateJSON(value);
            ASSERT_EQUALS(lastBSONElement().Number(), value);
        };

        test(std::numeric_limits<T>::max());
        test(std::numeric_limits<T>::min());
        test(std::numeric_limits<T>::lowest());
        test(static_cast<T>(-10));
        test(static_cast<T>(-2));
        test(static_cast<T>(-1));
        test(static_cast<T>(0));
        test(static_cast<T>(1));
        test(static_cast<T>(2));
        test(static_cast<T>(10));
    };

    bool b = true;
    LOGV2(20014, "bool {name}", "name"_attr = b);
    ASSERT_EQUALS(text.back(), "bool true");
    validateJSON(b);
    ASSERT(lastBSONElement().Bool() == b);

    char c = 1;
    LOGV2(20015, "char {name}", "name"_attr = c);
    ASSERT_EQUALS(text.back(), "char 1");
    validateJSON(static_cast<uint8_t>(
        c));  // cast, boost property_tree will try and parse as ascii otherwise
    ASSERT(lastBSONElement().Number() == c);

    testIntegral(static_cast<signed char>(0));
    testIntegral(static_cast<unsigned char>(0));
    testIntegral(static_cast<short>(0));
    testIntegral(static_cast<unsigned short>(0));
    testIntegral(0);
    testIntegral(0u);
    testIntegral(0l);
    testIntegral(0ul);
    testIntegral(0ll);
    testIntegral(0ull);
    testIntegral(static_cast<int64_t>(0));
    testIntegral(static_cast<uint64_t>(0));
    testIntegral(static_cast<size_t>(0));
    testFloatingPoint(0.0f);
    testFloatingPoint(0.0);

    // long double is prohibited, we don't use this type and favors Decimal128 instead.

    // enums

    enum UnscopedEnum { UnscopedEntry };
    LOGV2(20076, "{name}", "name"_attr = UnscopedEntry);
    auto expectedUnscoped = static_cast<std::underlying_type_t<UnscopedEnum>>(UnscopedEntry);
    ASSERT_EQUALS(text.back(), std::to_string(expectedUnscoped));
    validateJSON(expectedUnscoped);
    ASSERT_EQUALS(lastBSONElement().Number(), expectedUnscoped);

    enum class ScopedEnum { Entry = -1 };
    LOGV2(20077, "{name}", "name"_attr = ScopedEnum::Entry);
    auto expectedScoped = static_cast<std::underlying_type_t<ScopedEnum>>(ScopedEnum::Entry);
    ASSERT_EQUALS(text.back(), std::to_string(expectedScoped));
    validateJSON(expectedScoped);
    ASSERT_EQUALS(lastBSONElement().Number(), expectedScoped);

    LOGV2(20078, "{name}", "name"_attr = UnscopedEntryWithToString);
    ASSERT_EQUALS(text.back(), toString(UnscopedEntryWithToString));
    validateJSON(toString(UnscopedEntryWithToString));
    ASSERT_EQUALS(lastBSONElement().String(), toString(UnscopedEntryWithToString));


    // string types
    const char* c_str = "a c string";
    LOGV2(20016, "c string {name}", "name"_attr = c_str);
    ASSERT_EQUALS(text.back(), "c string a c string");
    validateJSON(std::string(c_str));
    ASSERT_EQUALS(lastBSONElement().String(), c_str);

    char* c_str2 = const_cast<char*>("non-const");
    LOGV2(20017, "c string {name}", "name"_attr = c_str2);
    ASSERT_EQUALS(text.back(), "c string non-const");
    validateJSON(std::string(c_str2));
    ASSERT_EQUALS(lastBSONElement().String(), c_str2);

    std::string str = "a std::string";
    LOGV2(20018, "std::string {name}", "name"_attr = str);
    ASSERT_EQUALS(text.back(), "std::string a std::string");
    validateJSON(str);
    ASSERT_EQUALS(lastBSONElement().String(), str);

    StringData str_data = "a StringData"_sd;
    LOGV2(20019, "StringData {name}", "name"_attr = str_data);
    ASSERT_EQUALS(text.back(), "StringData a StringData");
    validateJSON(str_data.toString());
    ASSERT_EQUALS(lastBSONElement().String(), str_data);

    {
        std::string_view s = "a std::string_view";
        LOGV2(4329200, "std::string_view {name}", "name"_attr = s);
        ASSERT_EQUALS(text.back(), "std::string_view a std::string_view");
        validateJSON(std::string{s});
        ASSERT_EQUALS(lastBSONElement().String(), s);
    }

    // BSONObj
    BSONObjBuilder builder;
    builder.append("int32"_sd, 1);
    builder.append("int64"_sd, std::numeric_limits<int64_t>::max());
    builder.append("double"_sd, 1.0);
    builder.append("str"_sd, str_data);
    BSONObj bsonObj = builder.obj();
    LOGV2(20020, "bson {name}", "name"_attr = bsonObj);
    ASSERT(text.back() ==
           std::string("bson ") + bsonObj.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonObj) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonObj) == 0);

    // BSONArray
    BSONArrayBuilder arrBuilder;
    arrBuilder.append("first"_sd);
    arrBuilder.append("second"_sd);
    arrBuilder.append("third"_sd);
    BSONArray bsonArr = arrBuilder.arr();
    LOGV2(20021, "{name}", "name"_attr = bsonArr);
    ASSERT_EQUALS(text.back(),
                  bsonArr.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonArr) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonArr) == 0);

    // BSONElement
    LOGV2(20022, "bson element {name}", "name"_attr = bsonObj.getField("int32"_sd));
    ASSERT(text.back() == std::string("bson element ") + bsonObj.getField("int32"_sd).toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name"_sd)
               .Obj()
               .getField("int32"_sd)
               .Int() == bsonObj.getField("int32"_sd).Int());
    ASSERT(lastBSONElement().Obj().getField("int32"_sd).Int() ==
           bsonObj.getField("int32"_sd).Int());

    // Date_t
    bool prevIsLocalTimezone = dateFormatIsLocalTimezone();
    for (auto localTimezone : {true, false}) {
        setDateFormatIsLocalTimezone(localTimezone);
        Date_t date = Date_t::now();
        LOGV2(20023, "Date_t {name}", "name"_attr = date);
        ASSERT_EQUALS(text.back(), std::string("Date_t ") + date.toString());
        ASSERT_EQUALS(mongo::fromjson(json.back())
                          .getField(kAttributesFieldName)
                          .Obj()
                          .getField("name")
                          .Date(),
                      date);
        ASSERT_EQUALS(lastBSONElement().Date(), date);
    }

    setDateFormatIsLocalTimezone(prevIsLocalTimezone);

    // Decimal128
    LOGV2(20024, "Decimal128 {name}", "name"_attr = Decimal128::kPi);
    ASSERT_EQUALS(text.back(), std::string("Decimal128 ") + Decimal128::kPi.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Decimal()
               .isEqual(Decimal128::kPi));
    ASSERT(lastBSONElement().Decimal().isEqual(Decimal128::kPi));

    // OID
    OID oid = OID::gen();
    LOGV2(20025, "OID {name}", "name"_attr = oid);
    ASSERT_EQUALS(text.back(), std::string("OID ") + oid.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").OID(),
        oid);
    ASSERT_EQUALS(lastBSONElement().OID(), oid);

    // Timestamp
    Timestamp ts = Timestamp::max();
    LOGV2(20026, "Timestamp {name}", "name"_attr = ts);
    ASSERT_EQUALS(text.back(), std::string("Timestamp ") + ts.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name")
                      .timestamp(),
                  ts);
    ASSERT_EQUALS(lastBSONElement().timestamp(), ts);

    // UUID
    UUID uuid = UUID::gen();
    LOGV2(20027, "UUID {name}", "name"_attr = uuid);
    ASSERT_EQUALS(text.back(), std::string("UUID ") + uuid.toString());
    ASSERT_EQUALS(UUID::parse(mongo::fromjson(json.back())
                                  .getField(kAttributesFieldName)
                                  .Obj()
                                  .getField("name")
                                  .Obj()),
                  uuid);
    ASSERT_EQUALS(UUID::parse(lastBSONElement().Obj()), uuid);

    // boost::optional
    LOGV2(20028, "boost::optional empty {name}", "name"_attr = boost::optional<bool>());
    ASSERT_EQUALS(text.back(),
                  std::string("boost::optional empty ") +
                      constants::kNullOptionalString.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .isNull());
    ASSERT(lastBSONElement().isNull());

    LOGV2(20029, "boost::optional<bool> {name}", "name"_attr = boost::optional<bool>(true));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<bool> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    LOGV2(20030,
          "boost::optional<boost::optional<bool>> {name}",
          "name"_attr = boost::optional<boost::optional<bool>>(boost::optional<bool>(true)));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<boost::optional<bool>> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    TypeWithBSON withBSON(1.0, 2.0);
    LOGV2(20031,
          "boost::optional<TypeWithBSON> {name}",
          "name"_attr = boost::optional<TypeWithBSON>(withBSON));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<TypeWithBSON> ") + withBSON.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(withBSON.toBSON()) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(withBSON.toBSON()) == 0);

    TypeWithoutBSON withoutBSON(1.0, 2.0);
    LOGV2(20032,
          "boost::optional<TypeWithBSON> {name}",
          "name"_attr = boost::optional<TypeWithoutBSON>(withoutBSON));
    ASSERT_EQUALS(text.back(),
                  std::string("boost::optional<TypeWithBSON> ") + withoutBSON.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").String(),
        withoutBSON.toString());
    ASSERT_EQUALS(lastBSONElement().String(), withoutBSON.toString());

    // Duration
    Milliseconds ms{12345};
    LOGV2(20033, "Duration {name}", "name"_attr = ms);
    ASSERT_EQUALS(text.back(), std::string("Duration ") + ms.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name" + ms.mongoUnitSuffix())
                      .Int(),
                  ms.count());
    ASSERT_EQUALS(BSONObj(bson.back().data())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name" + ms.mongoUnitSuffix())
                      .Long(),
                  ms.count());
}

TEST_F(LogTestV2, TextFormat) {
    std::vector<std::string> lines;
    auto sink = LogCaptureBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(TextFormatter());
    attach(sink);

    LOGV2_OPTIONS(20065, {LogTag::kNone}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") == std::string::npos);

    LOGV2_OPTIONS(20066, {LogTag::kStartupWarnings}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    LOGV2_OPTIONS(20067,
                  {static_cast<LogTag::Value>(LogTag::kStartupWarnings | LogTag::kPlainShell)},
                  "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    TypeWithBSON t(1.0, 2.0);
    LOGV2(20034, "{name} custom formatting", "name"_attr = t);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting") != std::string::npos);

    LOGV2(20035, "{name} bson", "name"_attr = t.toBSON());
    ASSERT(lines.back().rfind(t.toBSON().jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0) +
                              " bson") != std::string::npos);

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20036, "{name} custom formatting, no bson", "name"_attr = t2);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting, no bson") != std::string::npos);

    TypeWithNonMemberFormatting t3;
    LOGV2(20079, "{name}", "name"_attr = t3);
    ASSERT(lines.back().rfind(toString(t3)) != std::string::npos);
}

std::string hello() {
    return "hello";
}

TEST_F(LogTestV2, JsonBsonFormat) {
    using namespace constants;

    std::vector<std::string> lines;
    auto sink = LogCaptureBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(JSONFormatter());
    attach(sink);

    std::vector<std::string> linesBson;
    auto sinkBson = LogCaptureBackend::create(linesBson);
    sinkBson->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    sinkBson->set_formatter(BSONFormatter());
    attach(sinkBson);

    BSONObj log;

    LOGV2(20037, "test");
    auto validateRoot = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kTimestampFieldName).Date(), Date_t::lastNowForTest());
        ASSERT_EQUALS(obj.getField(kSeverityFieldName).String(),
                      LogSeverity::Info().toStringDataCompact());
        ASSERT_EQUALS(obj.getField(kComponentFieldName).String(),
                      LogComponent(MONGO_LOGV2_DEFAULT_COMPONENT).getNameForLog());
        ASSERT_EQUALS(obj.getField(kContextFieldName).String(), getThreadName());
        ASSERT_EQUALS(obj.getField(kIdFieldName).Int(), 20037);
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test");
        ASSERT(!obj.hasField(kAttributesFieldName));
        ASSERT(!obj.hasField(kTagsFieldName));
    };
    validateRoot(mongo::fromjson(lines.back()));
    validateRoot(BSONObj(linesBson.back().data()));


    LOGV2(20038, "test {name}", "name"_attr = 1);
    auto validateAttr = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 1);
    };
    validateAttr(mongo::fromjson(lines.back()));
    validateAttr(BSONObj(linesBson.back().data()));


    LOGV2(20039, "test {name:d}", "name"_attr = 2);
    auto validateMsgReconstruction = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name:d}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    };
    validateMsgReconstruction(mongo::fromjson(lines.back()));
    validateMsgReconstruction(BSONObj(linesBson.back().data()));

    LOGV2(20040, "test {name: <4}", "name"_attr = 2);
    auto validateMsgReconstruction2 = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name: <4}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    };
    validateMsgReconstruction2(mongo::fromjson(lines.back()));
    validateMsgReconstruction2(BSONObj(linesBson.back().data()));


    LOGV2_OPTIONS(20068, {LogTag::kStartupWarnings}, "warning");
    auto validateTags = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "warning");
        ASSERT_EQUALS(
            obj.getField("tags"_sd).Obj().woCompare(LogTag(LogTag::kStartupWarnings).toBSONArray()),
            0);
    };
    validateTags(mongo::fromjson(lines.back()));
    validateTags(BSONObj(linesBson.back().data()));

    LOGV2_OPTIONS(20069, {LogComponent::kControl}, "different component");
    auto validateComponent = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField("c"_sd).String(),
                      LogComponent(LogComponent::kControl).getNameForLog());
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "different component");
    };
    validateComponent(mongo::fromjson(lines.back()));
    validateComponent(BSONObj(linesBson.back().data()));


    TypeWithBSON t(1.0, 2.0);
    LOGV2(20041, "{name} custom formatting", "name"_attr = t);
    auto validateCustomAttr = [&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    };
    validateCustomAttr(mongo::fromjson(lines.back()));
    validateCustomAttr(BSONObj(linesBson.back().data()));


    LOGV2(20042, "{name} bson", "name"_attr = t.toBSON());
    auto validateBsonAttr = [&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} bson");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    };
    validateBsonAttr(mongo::fromjson(lines.back()));
    validateBsonAttr(BSONObj(linesBson.back().data()));


    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20043, "{name} custom formatting", "name"_attr = t2);
    auto validateCustomAttrWithoutBSON = [&t2](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").String(),
                      t2.toString());
    };
    validateCustomAttrWithoutBSON(mongo::fromjson(lines.back()));
    validateCustomAttrWithoutBSON(BSONObj(linesBson.back().data()));

    TypeWithBSONSerialize t3(1.0, 2.0);
    LOGV2(20044, "{name}", "name"_attr = t3);
    auto validateCustomAttrBSONSerialize = [&t3](const BSONObj& obj) {
        BSONObjBuilder builder;
        t3.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    };
    validateCustomAttrBSONSerialize(mongo::fromjson(lines.back()));
    validateCustomAttrBSONSerialize(BSONObj(linesBson.back().data()));


    TypeWithBothBSONFormatters t4(1.0, 2.0);
    LOGV2(20045, "{name}", "name"_attr = t4);
    auto validateCustomAttrBSONBothFormatters = [&t4](const BSONObj& obj) {
        BSONObjBuilder builder;
        t4.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    };
    validateCustomAttrBSONBothFormatters(mongo::fromjson(lines.back()));
    validateCustomAttrBSONBothFormatters(BSONObj(linesBson.back().data()));

    TypeWithBSONArray t5;
    LOGV2(20046, "{name}", "name"_attr = t5);
    auto validateCustomAttrBSONArray = [&t5](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").type(),
                      BSONType::Array);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(t5.toBSONArray()) == 0);
    };
    validateCustomAttrBSONArray(mongo::fromjson(lines.back()));
    validateCustomAttrBSONArray(BSONObj(linesBson.back().data()));

    TypeWithNonMemberFormatting t6;
    LOGV2(20080, "{name}", "name"_attr = t6);
    auto validateNonMemberToBSON = [&t6](const BSONObj& obj) {
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(toBSON(t6)) ==
            0);
    };
    validateNonMemberToBSON(mongo::fromjson(lines.back()));
    validateNonMemberToBSON(BSONObj(linesBson.back().data()));

    DynamicAttributes attrs;
    attrs.add("string data", "a string data"_sd);
    attrs.add("cstr", "a c string");
    attrs.add("int", 5);
    attrs.add("float", 3.0f);
    attrs.add("bool", true);
    attrs.add("enum", UnscopedEntryWithToString);
    attrs.add("custom", t6);
    attrs.addUnsafe("unsafe but ok", 1);
    BSONObj bsonObj;
    attrs.add("bson", bsonObj);
    attrs.add("millis", Milliseconds(1));
    attrs.addDeepCopy("stdstr", hello());
    LOGV2(20083, "message", attrs);
    auto validateDynamic = [](const BSONObj& obj) {
        const BSONObj& attrObj = obj.getField(kAttributesFieldName).Obj();
        for (StringData f : {"cstr"_sd,
                             "int"_sd,
                             "float"_sd,
                             "bool"_sd,
                             "enum"_sd,
                             "custom"_sd,
                             "bson"_sd,
                             "millisMillis"_sd,
                             "stdstr"_sd,
                             "unsafe but ok"_sd}) {
            ASSERT(attrObj.hasField(f));
        }

        // Check that one of them actually has the value too.
        ASSERT_EQUALS(attrObj.getField("int").Int(), 5);
    };
    validateDynamic(mongo::fromjson(lines.back()));
    validateDynamic(BSONObj(linesBson.back().data()));
}

TEST_F(LogTestV2, Containers) {
    using namespace constants;

    std::vector<std::string> text;
    auto text_sink = LogCaptureBackend::create(text);
    text_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    text_sink->set_formatter(PlainFormatter());
    attach(text_sink);

    std::vector<std::string> json;
    auto json_sink = LogCaptureBackend::create(json);
    json_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    json_sink->set_formatter(JSONFormatter());
    attach(json_sink);

    std::vector<std::string> bson;
    auto bson_sink = LogCaptureBackend::create(bson);
    bson_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    bson_sink->set_formatter(BSONFormatter());
    attach(bson_sink);

    // Helper to create a comma separated list of a container, stringify is function on how to
    // transform element into a string.
    auto text_join = [](auto begin, auto end, auto stringify) -> std::string {
        if (begin == end)
            return "()";

        auto second = begin;
        ++second;
        if (second == end)
            return fmt::format("({})", stringify(*begin));

        return fmt::format(
            "({})",
            std::accumulate(
                second, end, stringify(*begin), [&stringify](std::string result, auto&& item) {
                    return result + ", " + stringify(item);
                }));
    };

    // All standard sequential containers are supported
    {
        std::vector<std::string> vectorStrings = {"str1", "str2", "str3"};
        LOGV2(20047, "{name}", "name"_attr = vectorStrings);
        ASSERT_EQUALS(text.back(),
                      text_join(vectorStrings.begin(),
                                vectorStrings.end(),
                                [](const std::string& str) { return str; }));
        auto validateStringVector = [&vectorStrings](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("name").Array();
            ASSERT_EQUALS(vectorStrings.size(), jsonVector.size());
            for (std::size_t i = 0; i < vectorStrings.size(); ++i)
                ASSERT_EQUALS(jsonVector[i].String(), vectorStrings[i]);
        };
        validateStringVector(mongo::fromjson(json.back()));
        validateStringVector(BSONObj(bson.back().data()));
    }

    {
        // Test that containers can contain uint32_t, even as this type is not BSON appendable
        std::vector<uint32_t> vectorUInt32s = {0, 1, std::numeric_limits<uint32_t>::max()};
        LOGV2(4684000, "{vectorUInt32s}", "vectorUInt32s"_attr = vectorUInt32s);
        auto validateUInt32Vector = [&vectorUInt32s](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("vectorUInt32s").Array();
            ASSERT_EQUALS(vectorUInt32s.size(), jsonVector.size());
            for (std::size_t i = 0; i < vectorUInt32s.size(); ++i) {
                const auto& jsonElem = jsonVector[i];
                if (jsonElem.type() == NumberInt)
                    ASSERT_EQUALS(jsonElem.Int(), vectorUInt32s[i]);
                else if (jsonElem.type() == NumberLong)
                    ASSERT_EQUALS(jsonElem.Long(), vectorUInt32s[i]);
                else
                    ASSERT(false) << "Element type is " << typeName(jsonElem.type())
                                  << ". Expected Int or Long.";
            }
        };
        validateUInt32Vector(mongo::fromjson(json.back()));
        validateUInt32Vector(BSONObj(bson.back().data()));
    }

    {
        // Elements can require custom formatting
        std::list<TypeWithBSON> listCustom = {
            TypeWithBSON(0.0, 1.0), TypeWithBSON(2.0, 3.0), TypeWithBSON(4.0, 5.0)};
        LOGV2(20048, "{name}", "name"_attr = listCustom);
        ASSERT_EQUALS(text.back(),
                      text_join(listCustom.begin(), listCustom.end(), [](const auto& item) {
                          return item.toString();
                      }));
        auto validateBSONObjList = [&listCustom](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("name").Array();
            ASSERT_EQUALS(listCustom.size(), jsonVector.size());
            auto in = listCustom.begin();
            auto out = jsonVector.begin();
            for (; in != listCustom.end(); ++in, ++out) {
                ASSERT(in->toBSON().woCompare(out->Obj()) == 0);
            }
        };
        validateBSONObjList(mongo::fromjson(json.back()));
        validateBSONObjList(BSONObj(bson.back().data()));
    }

    {
        // Optionals are also allowed as elements
        std::forward_list<boost::optional<bool>> listOptionalBool = {true, boost::none, false};
        LOGV2(20049, "{name}", "name"_attr = listOptionalBool);
        ASSERT_EQUALS(text.back(),
                      text_join(listOptionalBool.begin(),
                                listOptionalBool.end(),
                                [](const auto& item) -> std::string {
                                    if (!item)
                                        return constants::kNullOptionalString.toString();
                                    else if (*item)
                                        return "true";
                                    else
                                        return "false";
                                }));
        auto validateOptionalBool = [&listOptionalBool](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("name").Array();
            auto in = listOptionalBool.begin();
            auto out = jsonVector.begin();
            for (; in != listOptionalBool.end() && out != jsonVector.end(); ++in, ++out) {
                if (*in)
                    ASSERT_EQUALS(**in, out->Bool());
                else
                    ASSERT(out->isNull());
            }
            ASSERT(in == listOptionalBool.end());
            ASSERT(out == jsonVector.end());
        };
        validateOptionalBool(mongo::fromjson(json.back()));
        validateOptionalBool(BSONObj(bson.back().data()));
    }

    {
        // Containers can be nested
        std::array<std::deque<int>, 4> arrayOfDeques = {{{0, 1}, {2, 3}, {4, 5}, {6, 7}}};
        LOGV2(20050, "{name}", "name"_attr = arrayOfDeques);
        ASSERT_EQUALS(text.back(),
                      text_join(arrayOfDeques.begin(),
                                arrayOfDeques.end(),
                                [text_join](const std::deque<int>& deque) {
                                    return text_join(deque.begin(), deque.end(), [](int val) {
                                        return fmt::format("{}", val);
                                    });
                                }));
        auto validateArrayOfDeques = [&arrayOfDeques](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("name").Array();
            ASSERT_EQUALS(arrayOfDeques.size(), jsonVector.size());
            auto in = arrayOfDeques.begin();
            auto out = jsonVector.begin();
            for (; in != arrayOfDeques.end(); ++in, ++out) {
                std::vector<BSONElement> inner_array = out->Array();
                ASSERT_EQUALS(in->size(), inner_array.size());
                auto inner_begin = in->begin();
                auto inner_end = in->end();

                auto inner_out = inner_array.begin();
                for (; inner_begin != inner_end; ++inner_begin, ++inner_out) {
                    ASSERT_EQUALS(*inner_begin, inner_out->Int());
                }
            }
        };
        validateArrayOfDeques(mongo::fromjson(json.back()));
        validateArrayOfDeques(BSONObj(bson.back().data()));
    }

    {
        // Associative containers are also supported
        std::map<std::string, std::string> mapStrStr = {{"key1", "val1"}, {"key2", "val2"}};
        LOGV2(20051, "{name}", "name"_attr = mapStrStr);
        ASSERT_EQUALS(text.back(),
                      text_join(mapStrStr.begin(), mapStrStr.end(), [](const auto& item) {
                          return fmt::format("{}: {}", item.first, item.second);
                      }));
        auto validateMapOfStrings = [&mapStrStr](const BSONObj& obj) {
            BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
            auto in = mapStrStr.begin();
            for (; in != mapStrStr.end(); ++in) {
                ASSERT_EQUALS(mappedValues.getField(in->first).String(), in->second);
            }
        };
        validateMapOfStrings(mongo::fromjson(json.back()));
        validateMapOfStrings(BSONObj(bson.back().data()));
    }

    {
        // Associative containers with optional sequential container is ok too
        stdx::unordered_map<std::string, boost::optional<std::vector<int>>> mapOptionalVector = {
            {"key1", boost::optional<std::vector<int>>{{1, 2, 3}}},
            {"key2", boost::optional<std::vector<int>>{boost::none}}};

        LOGV2(20052, "{name}", "name"_attr = mapOptionalVector);
        ASSERT_EQUALS(
            text.back(),
            text_join(mapOptionalVector.begin(),
                      mapOptionalVector.end(),
                      [text_join](const std::pair<std::string, boost::optional<std::vector<int>>>&
                                      optionalVectorItem) {
                          if (!optionalVectorItem.second)
                              return optionalVectorItem.first + ": " +
                                  constants::kNullOptionalString.toString();
                          else
                              return optionalVectorItem.first + ": " +
                                  text_join(optionalVectorItem.second->begin(),
                                            optionalVectorItem.second->end(),
                                            [](int val) { return fmt::format("{}", val); });
                      }));
        auto validateMapOfOptionalVectors = [&mapOptionalVector](const BSONObj& obj) {
            BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
            auto in = mapOptionalVector.begin();
            for (; in != mapOptionalVector.end(); ++in) {
                BSONElement mapElement = mappedValues.getField(in->first);
                if (!in->second)
                    ASSERT(mapElement.isNull());
                else {
                    const std::vector<int>& intVec = *(in->second);
                    std::vector<BSONElement> jsonVector = mapElement.Array();
                    ASSERT_EQUALS(jsonVector.size(), intVec.size());
                    for (std::size_t i = 0; i < intVec.size(); ++i)
                        ASSERT_EQUALS(jsonVector[i].Int(), intVec[i]);
                }
            }
        };
        validateMapOfOptionalVectors(mongo::fromjson(json.back()));
        validateMapOfOptionalVectors(BSONObj(bson.back().data()));
    }

    {
        std::vector<Nanoseconds> nanos = {Nanoseconds(10), Nanoseconds(100)};
        LOGV2(20081, "{name}", "name"_attr = nanos);
        auto validateDurationVector = [&nanos](const BSONObj& obj) {
            std::vector<BSONElement> jsonVector =
                obj.getField(kAttributesFieldName).Obj().getField("name").Array();
            ASSERT_EQUALS(nanos.size(), jsonVector.size());
            for (std::size_t i = 0; i < nanos.size(); ++i)
                ASSERT(jsonVector[i].Obj().woCompare(nanos[i].toBSON()) == 0);
        };
        validateDurationVector(mongo::fromjson(json.back()));
        validateDurationVector(BSONObj(bson.back().data()));
    }

    {
        std::map<std::string, Microseconds> mapOfMicros = {{"first", Microseconds(20)},
                                                           {"second", Microseconds(40)}};
        LOGV2(20082, "{name}", "name"_attr = mapOfMicros);
        auto validateMapOfMicros = [&mapOfMicros](const BSONObj& obj) {
            BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
            auto in = mapOfMicros.begin();
            for (; in != mapOfMicros.end(); ++in) {
                ASSERT(mappedValues.getField(in->first).Obj().woCompare(in->second.toBSON()) == 0);
            }
        };
        validateMapOfMicros(mongo::fromjson(json.back()));
        validateMapOfMicros(BSONObj(bson.back().data()));
    }

    {
        // Test that maps can contain uint32_t, even as this type is not BSON appendable
        StringMap<uint32_t> mapOfUInt32s = {
            {"first", 0}, {"second", 1}, {"third", std::numeric_limits<uint32_t>::max()}};
        LOGV2(4684001, "{mapOfUInt32s}", "mapOfUInt32s"_attr = mapOfUInt32s);
        auto validateMapOfUInt32s = [&mapOfUInt32s](const BSONObj& obj) {
            BSONObj mappedValues =
                obj.getField(kAttributesFieldName).Obj().getField("mapOfUInt32s").Obj();
            for (const auto& mapElem : mapOfUInt32s) {
                auto elem = mappedValues.getField(mapElem.first);
                if (elem.type() == NumberInt)
                    ASSERT_EQUALS(elem.Int(), mapElem.second);
                else if (elem.type() == NumberLong)
                    ASSERT_EQUALS(elem.Long(), mapElem.second);
                else
                    ASSERT(false) << "Element type is " << typeName(elem.type())
                                  << ". Expected Int or Long.";
            }
        };
        validateMapOfUInt32s(mongo::fromjson(json.back()));
        validateMapOfUInt32s(BSONObj(bson.back().data()));
    }
}

TEST_F(LogTestV2, Unicode) {
    std::vector<std::string> lines;
    auto sink = LogCaptureBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(JSONFormatter());
    attach(sink);

    // JSON requires strings to be valid UTF-8 and control characters escaped.
    // JSON parsers decode escape sequences so control characters should be round-trippable.
    // Invalid UTF-8 encoded data is replaced by the Unicode Replacement Character (U+FFFD).
    // There is no way to preserve the data without introducing special semantics in how to parse.
    std::pair<StringData, StringData> strs[] = {
        // Single byte characters that needs to be escaped
        {"\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd, "\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd},
        // multi byte characters that needs to be escaped (unicode control characters)
        {"\u0080\u009f"_sd, "\u0080\u009f"_sd},
        // Valid 2 Octet sequence, LATIN SMALL LETTER N WITH TILDE
        {"\u00f1"_sd, "\u00f1"_sd},
        // Invalid 2 Octet Sequence, result is escaped
        {"\xc3\x28"_sd, "\ufffd\x28"_sd},
        // Invalid Sequence Identifier, result is escaped
        {"\xa0\xa1"_sd, "\ufffd\ufffd"_sd},
        // Valid 3 Octet sequence, RUNIC LETTER TIWAZ TIR TYR T
        {"\u16cf"_sd, "\u16cf"_sd},
        // Invalid 3 Octet Sequence (in 2nd Octet), result is escaped
        {"\xe2\x28\xa1"_sd, "\ufffd\x28\ufffd"_sd},
        // Invalid 3 Octet Sequence (in 3rd Octet), result is escaped
        {"\xe2\x82\x28"_sd, "\ufffd\ufffd\x28"_sd},
        // Valid 4 Octet sequence, GOTHIC LETTER MANNA
        {"\U0001033c"_sd, "\U0001033c"_sd},
        // Invalid 4 Octet Sequence (in 2nd Octet), result is escaped
        {"\xf0\x28\x8c\xbc"_sd, "\ufffd\x28\ufffd\ufffd"_sd},
        // Invalid 4 Octet Sequence (in 3rd Octet), result is escaped
        {"\xf0\x90\x28\xbc"_sd, "\ufffd\ufffd\x28\ufffd"_sd},
        // Invalid 4 Octet Sequence (in 4th Octet), result is escaped
        {"\xf0\x28\x8c\x28"_sd, "\ufffd\x28\ufffd\x28"_sd},
        // Valid 5 Octet Sequence (but not Unicode!), result is escaped
        {"\xf8\xa1\xa1\xa1\xa1"_sd, "\ufffd\ufffd\ufffd\ufffd\ufffd"_sd},
        // Valid 6 Octet Sequence (but not Unicode!), result is escaped
        {"\xfc\xa1\xa1\xa1\xa1\xa1"_sd, "\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd"_sd},
        // Invalid 3 Octet sequence, buffer ends prematurely, result is escaped
        {"\xe2\x82"_sd, "\ufffd\ufffd"_sd},
    };

    auto getLastMongo = [&]() {
        return mongo::fromjson(lines.back())
            .getField(constants::kAttributesFieldName)
            .Obj()
            .getField("name")
            .String();
    };

    auto getLastPtree = [&]() {
        namespace pt = boost::property_tree;

        std::istringstream json_stream(lines.back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        return ptree.get<std::string>(std::string(constants::kAttributesFieldName) + ".name");
    };

    for (const auto& pair : strs) {
        LOGV2(20053, "{name}", "name"_attr = pair.first);

        // Verify with both our parser and boost::property_tree
        ASSERT_EQUALS(pair.second, getLastMongo());
        ASSERT_EQUALS(pair.second, getLastPtree());
    }
}

TEST_F(LogTestV2, JsonTruncation) {
    using namespace constants;

    std::vector<std::string> lines;
    auto sink = LogCaptureBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(JSONFormatter());
    attach(sink);

    std::size_t maxAttributeOutputSize = constants::kDefaultMaxAttributeOutputSizeKB * 1024;

    BSONObjBuilder builder;
    BSONObjBuilder subobj = builder.subobjStart("sub"_sd);
    subobj.append("small1", 1);
    subobj.append("small2", "small string");
    subobj.append("large", std::string(maxAttributeOutputSize * 2, 'a'));
    subobj.append("small3", "small string after large object");
    subobj.done();

    LOGV2(20085, "{name}{attr2}", "name"_attr = builder.done(), "attr2"_attr = true);
    auto validateTruncation = [&](const BSONObj& obj) {
        // Check that all fields up until the large one is written
        BSONObj sub = obj.getField(constants::kAttributesFieldName)
                          .Obj()
                          .getField("name"_sd)
                          .Obj()
                          .getField("sub"_sd)
                          .Obj();
        ASSERT(sub.hasField("small1"_sd));
        ASSERT(sub.hasField("small2"_sd));
        ASSERT(!sub.hasField("large"_sd));
        ASSERT(!sub.hasField("small3"_sd));

        // The truncated field should we witten in the truncated and size sub object
        BSONObj truncated = obj.getField(constants::kTruncatedFieldName).Obj();
        BSONObj truncatedInfo =
            truncated.getField("name"_sd).Obj().getField("sub"_sd).Obj().getField("large"_sd).Obj();
        ASSERT_EQUALS(truncatedInfo.getField("type"_sd).String(), typeName(BSONType::String));
        ASSERT(truncatedInfo.getField("size"_sd).isNumber());

        ASSERT_EQUALS(
            obj.getField(constants::kTruncatedSizeFieldName).Obj().getField("name"_sd).Int(),
            builder.done().objsize());

        // Attributes coming after the truncated one should be written
        ASSERT(obj.getField(constants::kAttributesFieldName).Obj().getField("attr2").Bool());
    };
    validateTruncation(mongo::fromjson(lines.back()));

    LOGV2_OPTIONS(20086, {LogTruncation::Disabled}, "{name}", "name"_attr = builder.done());
    auto validateTruncationDisabled = [&](const BSONObj& obj) {
        BSONObj sub = obj.getField(constants::kAttributesFieldName)
                          .Obj()
                          .getField("name"_sd)
                          .Obj()
                          .getField("sub"_sd)
                          .Obj();
        // No truncation should occur
        ASSERT(sub.hasField("small1"_sd));
        ASSERT(sub.hasField("small2"_sd));
        ASSERT(sub.hasField("large"_sd));
        ASSERT(sub.hasField("small3"_sd));

        ASSERT(!obj.hasField(constants::kTruncatedFieldName));
        ASSERT(!obj.hasField(constants::kTruncatedSizeFieldName));
    };
    validateTruncationDisabled(mongo::fromjson(lines.back()));

    BSONArrayBuilder arrBuilder;
    // Fields will use more than one byte each so this will truncate at some point
    for (size_t i = 0; i < maxAttributeOutputSize; ++i) {
        arrBuilder.append("str");
    }

    BSONArray arrToLog = arrBuilder.arr();
    LOGV2(20087, "{name}", "name"_attr = arrToLog);
    auto validateArrayTruncation = [&](const BSONObj& obj) {
        auto arr = obj.getField(constants::kAttributesFieldName).Obj().getField("name"_sd).Array();
        ASSERT_LESS_THAN(arr.size(), maxAttributeOutputSize);

        std::string truncatedFieldName = std::to_string(arr.size());
        BSONObj truncated = obj.getField(constants::kTruncatedFieldName).Obj();
        BSONObj truncatedInfo =
            truncated.getField("name"_sd).Obj().getField(truncatedFieldName).Obj();
        ASSERT_EQUALS(truncatedInfo.getField("type"_sd).String(), typeName(BSONType::String));
        ASSERT(truncatedInfo.getField("size"_sd).isNumber());

        ASSERT_EQUALS(
            obj.getField(constants::kTruncatedSizeFieldName).Obj().getField("name"_sd).Int(),
            arrToLog.objsize());
    };
    validateArrayTruncation(mongo::fromjson(lines.back()));
}

TEST_F(LogTestV2, Threads) {
    std::vector<std::string> linesPlain;
    auto plainSink = LogCaptureBackend::create(linesPlain);
    plainSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    plainSink->set_formatter(PlainFormatter());
    attach(plainSink);

    std::vector<std::string> linesText;
    auto textSink = LogCaptureBackend::create(linesText);
    textSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    textSink->set_formatter(TextFormatter());
    attach(textSink);

    std::vector<std::string> linesJson;
    auto jsonSink = LogCaptureBackend::create(linesJson);
    jsonSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    jsonSink->set_formatter(JSONFormatter());
    attach(jsonSink);

    constexpr int kNumPerThread = 1000;
    std::vector<stdx::thread> threads;

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20054, "thread1");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20055, "thread2");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20056, "thread3");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20057, "thread4");
    });

    for (auto&& thread : threads) {
        thread.join();
    }

    ASSERT(linesPlain.size() == threads.size() * kNumPerThread);
    ASSERT(linesText.size() == threads.size() * kNumPerThread);
    ASSERT(linesJson.size() == threads.size() * kNumPerThread);
}

TEST_F(LogTestV2, Ramlog) {
    RamLog* ramlog = RamLog::get("test_ramlog");

    auto sink = boost::make_shared<boost::log::sinks::unlocked_sink<RamLogSink>>(
        boost::make_shared<RamLogSink>(ramlog));
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    std::vector<std::string> lines;
    auto testSink = LogCaptureBackend::create(lines);
    testSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    testSink->set_formatter(PlainFormatter());
    attach(testSink);

    auto verifyRamLog = [&]() {
        RamLog::LineIterator iter(ramlog);
        return std::all_of(lines.begin(), lines.end(), [&iter](const std::string& line) {
            return line == iter.next();
        });
    };

    LOGV2(20058, "test");
    ASSERT(verifyRamLog());
    LOGV2(20059, "test2");
    ASSERT(verifyRamLog());
}

// Positive: Test that the ram log is properly circular
TEST_F(LogTestV2, Ramlog_CircularBuffer) {
    RamLog* ramlog = RamLog::get("test_ramlog2");

    std::vector<std::string> lines;

    constexpr size_t maxLines = 1024;
    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(i);
        lines.push_back(s);
        ramlog->write(s);
    }

    lines.erase(lines.begin(), lines.begin() + (testLines - maxLines) + 1);

    // Verify we circled correctly through the buffer
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), 5000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log has a max size cap
TEST_F(LogTestV2, Ramlog_MaxSize) {
    RamLog* ramlog = RamLog::get("test_ramlog3");

    std::vector<std::string> lines;

    constexpr size_t testLines = 2000;
    constexpr size_t longStringLength = 2048;

    std::string longStr(longStringLength, 'a');

    // Write enough lines to trigger wrapping and trimming
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(10000 + i) + longStr;
        lines.push_back(s);
        ramlog->write(s);
    }

    constexpr size_t linesToFit = (1024 * 1024) / (5 + longStringLength);

    lines.erase(lines.begin(), lines.begin() + (testLines - linesToFit));

    // Verify we keep just enough lines that fit
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), 2000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log handles really large lines
TEST_F(LogTestV2, Ramlog_GiantLine) {
    RamLog* ramlog = RamLog::get("test_ramlog4");

    std::vector<std::string> lines;

    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        ramlog->write(std::to_string(i));
    }

    auto s = std::to_string(testLines);
    lines.push_back(s);
    ramlog->write(s);

    std::string bigStr(2048 * 1024, 'a');
    lines.push_back(bigStr);
    ramlog->write(bigStr);

    // Verify we keep 2 lines
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), testLines + 2);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

TEST_F(LogTestV2, MultipleDomains) {
    std::vector<std::string> global_lines;
    auto sink = LogCaptureBackend::create(global_lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    // Example how a second domain can be created.
    class OtherDomainImpl : public LogDomain::Internal {
    public:
        OtherDomainImpl() {}

        LogSource& source() override {
            thread_local LogSource lg(this);
            return lg;
        }
    };

    LogDomain other_domain(std::make_unique<OtherDomainImpl>());
    std::vector<std::string> other_lines;
    auto other_sink = LogCaptureBackend::create(other_lines);
    other_sink->set_filter(
        ComponentSettingsFilter(other_domain, LogManager::global().getGlobalSettings()));
    other_sink->set_formatter(PlainFormatter());
    attach(other_sink);

    LOGV2_OPTIONS(20070, {&other_domain}, "test");
    ASSERT(global_lines.empty());
    ASSERT(other_lines.back() == "test");

    LOGV2(20060, "global domain log");
    ASSERT(global_lines.back() == "global domain log");
    ASSERT(other_lines.back() == "test");
}

TEST_F(LogTestV2, FileLogging) {
    auto logv2_dir = std::make_unique<mongo::unittest::TempDir>("logv2");

    // Examples of some capabilities for file logging. Rotation, header/footer support.
    std::string file_name = logv2_dir->path() + "/file.log";
    std::string rotated_file_name = logv2_dir->path() + "/file-rotated.log";

    auto backend = boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = file_name);
    backend->auto_flush();
    backend->set_open_handler(
        [](boost::log::sinks::text_file_backend::stream_type& file) { file << "header\n"; });
    backend->set_close_handler(
        [](boost::log::sinks::text_file_backend::stream_type& file) { file << "footer\n"; });

    auto sink = boost::make_shared<
        boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>>(backend);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    auto readFile = [&](std::string const& filename) {
        std::vector<std::string> lines;
        std::ifstream file(filename);
        char line[1000] = {'\0'};

        while (true) {
            file.getline(line, sizeof(line), '\n');
            if (file.good()) {
                lines.emplace_back(line);
            } else
                break;
        }

        return lines;
    };

    LOGV2(20061, "test");
    ASSERT(readFile(file_name).back() == "test");

    LOGV2(20062, "test2");
    ASSERT(readFile(file_name).back() == "test2");

    auto before_rotation = readFile(file_name);
    ASSERT(before_rotation.front() == "header");
    if (auto locked = sink->locked_backend()) {
        locked->set_target_file_name_pattern(rotated_file_name);
        locked->rotate_file();
    }

    ASSERT(readFile(file_name).empty());
    auto after_rotation = readFile(rotated_file_name);
    ASSERT(after_rotation.back() == "footer");
    before_rotation.push_back(after_rotation.back());
    ASSERT(before_rotation == after_rotation);
}

TEST_F(LogTestV2, UserAssert) {
    std::vector<std::string> lines;

    using backend_t = CompositeBackend<LogCaptureBackend, UserAssertSink>;

    auto sink = boost::make_shared<boost::log::sinks::synchronous_sink<backend_t>>(
        boost::make_shared<backend_t>(boost::make_shared<LogCaptureBackend>(lines),
                                      boost::make_shared<UserAssertSink>()));

    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    bool gotUassert = false;
    try {
        LOGV2_OPTIONS(4652000, {UserAssertAfterLog(ErrorCodes::BadValue)}, "uasserting log");
    } catch (const DBException& ex) {
        ASSERT_EQUALS(ex.code(), ErrorCodes::BadValue);
        ASSERT_EQUALS(ex.reason(), "uasserting log");
        ASSERT_EQUALS(lines.back(), ex.reason());
        gotUassert = true;
    }
    ASSERT(gotUassert);


    bool gotUassertWithReplacementFields = false;
    try {
        LOGV2_OPTIONS(4652001,
                      {UserAssertAfterLog(ErrorCodes::BadValue)},
                      "uasserting log {name}",
                      "name"_attr = 1);
    } catch (const DBException& ex) {
        ASSERT_EQUALS(ex.code(), ErrorCodes::BadValue);
        ASSERT_EQUALS(ex.reason(), "uasserting log 1");
        ASSERT_EQUALS(lines.back(), ex.reason());
        gotUassertWithReplacementFields = true;
    }
    ASSERT(gotUassertWithReplacementFields);
}

}  // namespace
}  // namespace logv2
}  // namespace mongo

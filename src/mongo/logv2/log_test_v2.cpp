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
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/formatter_base.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/uuid.h"

#include <boost/log/attributes/constant.hpp>
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

struct TypeWithBSON : public TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("x"_sd, _x);
        builder.append("y"_sd, _y);
        return builder.obj();
    }
};

class LogTestBackend
    : public boost::log::sinks::
          basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
    LogTestBackend(std::vector<std::string>& lines) : _logLines(lines) {}

    static boost::shared_ptr<boost::log::sinks::synchronous_sink<LogTestBackend>> create(
        std::vector<std::string>& lines) {
        auto backend = boost::make_shared<LogTestBackend>(lines);
        return boost::make_shared<boost::log::sinks::synchronous_sink<LogTestBackend>>(
            std::move(backend));
    }

    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        _logLines.push_back(formatted_string);
    }

private:
    std::vector<std::string>& _logLines;
};

class LogDuringInitTester {
public:
    LogDuringInitTester() {
        std::vector<std::string> lines;
        auto sink = LogTestBackend::create(lines);
        sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
        sink->set_formatter(PlainFormatter());
        boost::log::core::get()->add_sink(sink);

        LOGV2("log during init");
        ASSERT(lines.back() == "log during init");

        boost::log::core::get()->remove_sink(sink);
    }
};

LogDuringInitTester logDuringInit;

TEST_F(LogTestV2, Basic) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    LOGV2("test");
    ASSERT(lines.back() == "test");

    LOGV2_DEBUG(-2, "test debug");
    ASSERT(lines.back() == "test debug");

    LOGV2("test {}", "name"_attr = 1);
    ASSERT(lines.back() == "test 1");

    LOGV2("test {:d}", "name"_attr = 2);
    ASSERT(lines.back() == "test 2");

    LOGV2("test {}", "name"_attr = "char*");
    ASSERT(lines.back() == "test char*");

    LOGV2("test {}", "name"_attr = std::string("std::string"));
    ASSERT(lines.back() == "test std::string");

    LOGV2("test {}", "name"_attr = "StringData"_sd);
    ASSERT(lines.back() == "test StringData");

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "test");
    ASSERT(lines.back() == "test");

    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT(lines.back() == t.toString() + " custom formatting");

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting, no bson", "name"_attr = t2);
    ASSERT(lines.back() == t.toString() + " custom formatting, no bson");
}

TEST_F(LogTestV2, Types) {
    std::vector<std::string> text;
    auto text_sink = LogTestBackend::create(text);
    text_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    text_sink->set_formatter(PlainFormatter());
    attach(text_sink);

    std::vector<std::string> json;
    auto json_sink = LogTestBackend::create(json);
    json_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    json_sink->set_formatter(JsonFormatter());
    attach(json_sink);

    // The JSON formatter should make the types round-trippable without data loss
    auto validateJSON = [&](auto expected) {
        namespace pt = boost::property_tree;

        std::istringstream json_stream(json.back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        ASSERT(ptree.get<decltype(expected)>("attr.name") == expected);
    };

    auto testNumeric = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text.clear();
            LOGV2("{}", "name"_attr = value);
            ASSERT_EQUALS(text.back(), fmt::format("{}", value));
            validateJSON(value);
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
    LOGV2("bool {}", "name"_attr = b);
    ASSERT_EQUALS(text.back(), "bool true");
    validateJSON(b);

    char c = 1;
    LOGV2("char {}", "name"_attr = c);
    ASSERT_EQUALS(text.back(), "char 1");
    validateJSON(static_cast<uint8_t>(
        c));  // cast, boost property_tree will try and parse as ascii otherwise

    testNumeric(static_cast<signed char>(0));
    testNumeric(static_cast<unsigned char>(0));
    testNumeric(static_cast<short>(0));
    testNumeric(static_cast<unsigned short>(0));
    testNumeric(0);
    testNumeric(0u);
    testNumeric(0l);
    testNumeric(0ul);
    testNumeric(0ll);
    testNumeric(0ull);
    testNumeric(static_cast<int64_t>(0));
    testNumeric(static_cast<uint64_t>(0));
    testNumeric(static_cast<size_t>(0));
    testNumeric(0.0f);
    testNumeric(0.0);

    // long double is prohibited, we don't use this type and favors Decimal128 instead.

    // string types
    const char* c_str = "a c string";
    LOGV2("c string {}", "name"_attr = c_str);
    ASSERT_EQUALS(text.back(), "c string a c string");
    validateJSON(std::string(c_str));

    std::string str = "a std::string";
    LOGV2("std::string {}", "name"_attr = str);
    ASSERT_EQUALS(text.back(), "std::string a std::string");
    validateJSON(str);

    StringData str_data = "a StringData"_sd;
    LOGV2("StringData {}", "name"_attr = str_data);
    ASSERT_EQUALS(text.back(), "StringData a StringData");
    validateJSON(str_data.toString());

    // BSONObj
    BSONObjBuilder builder;
    builder.append("int32"_sd, 0);
    builder.append("int64"_sd, std::numeric_limits<int64_t>::max());
    builder.append("double"_sd, 0.0);
    builder.append("str"_sd, str_data);
    BSONObj bson = builder.obj();
    LOGV2("bson {}", "name"_attr = bson);
    ASSERT_EQUALS(text.back(), std::string("bson ") + bson.jsonString());
    ASSERT(mongo::fromjson(json.back())
               .getField("attr"_sd)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bson) == 0);

    // Date_t
    Date_t date = Date_t::now();
    LOGV2("Date_t {}", "name"_attr = date);
    ASSERT_EQUALS(text.back(), std::string("Date_t ") + date.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back()).getField("attr").Obj().getField("name").Date(),
                  date);

    // Decimal128
    LOGV2("Decimal128 {}", "name"_attr = Decimal128::kPi);
    ASSERT_EQUALS(text.back(), std::string("Decimal128 ") + Decimal128::kPi.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField("attr")
               .Obj()
               .getField("name")
               .Decimal()
               .isEqual(Decimal128::kPi));

    // OID
    OID oid = OID::gen();
    LOGV2("OID {}", "name"_attr = oid);
    ASSERT_EQUALS(text.back(), std::string("OID ") + oid.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back()).getField("attr").Obj().getField("name").OID(), oid);

    // Timestamp
    Timestamp ts = Timestamp::max();
    LOGV2("Timestamp {}", "name"_attr = ts);
    ASSERT_EQUALS(text.back(), std::string("Timestamp ") + ts.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back()).getField("attr").Obj().getField("name").timestamp(),
                  ts);

    // UUID
    UUID uuid = UUID::gen();
    LOGV2("UUID {}", "name"_attr = uuid);
    ASSERT_EQUALS(text.back(), std::string("UUID ") + uuid.toString());
    ASSERT_EQUALS(
        UUID::parse(mongo::fromjson(json.back()).getField("attr").Obj().getField("name").Obj()),
        uuid);
}

TEST_F(LogTestV2, TextFormat) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(TextFormatter());
    attach(sink);

    LOGV2_OPTIONS({LogTag::kNone}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") == std::string::npos);

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    LOGV2_OPTIONS({static_cast<LogTag::Value>(LogTag::kStartupWarnings | LogTag::kJavascript)},
                  "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting") != std::string::npos);

    LOGV2("{} bson", "name"_attr = t.toBSON());
    ASSERT(lines.back().rfind(t.toBSON().jsonString() + " bson") != std::string::npos);

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting, no bson", "name"_attr = t2);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting, no bson") != std::string::npos);
}

TEST_F(LogTestV2, JSONFormat) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(JsonFormatter());
    attach(sink);

    BSONObj log;

    LOGV2("test");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("t"_sd).String() == dateToISOStringUTC(Date_t::lastNowForTest()));
    ASSERT(log.getField("s"_sd).String() == LogSeverity::Info().toStringDataCompact());
    ASSERT(log.getField("c"_sd).String() ==
           LogComponent(MONGO_LOGV2_DEFAULT_COMPONENT).getNameForLog());
    ASSERT(log.getField("ctx"_sd).String() == getThreadName());
    ASSERT(!log.hasField("id"_sd));
    ASSERT(log.getField("msg"_sd).String() == "test");
    ASSERT(!log.hasField("attr"_sd));
    ASSERT(!log.hasField("tags"_sd));

    LOGV2("test {}", "name"_attr = 1);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "test {name}");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Int() == 1);

    LOGV2("test {:d}", "name"_attr = 2);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "test {name:d}");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Int() == 2);

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "warning");
    ASSERT(log.getField("tags"_sd).Array().front().woCompare(
               mongo::fromjson(LogTag(LogTag::kStartupWarnings).toJSONArray())[0]) == 0);

    LOGV2_OPTIONS({LogComponent::kControl}, "different component");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("c"_sd).String() == LogComponent(LogComponent::kControl).getNameForLog());
    ASSERT(log.getField("msg"_sd).String() == "different component");

    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "{name} custom formatting");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Obj().woCompare(
               mongo::fromjson(t.toBSON().jsonString())) == 0);

    LOGV2("{} bson", "name"_attr = t.toBSON());
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "{name} bson");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Obj().woCompare(
               mongo::fromjson(t.toBSON().jsonString())) == 0);

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t2);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "{name} custom formatting");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").String() == t.toString());
}

TEST_F(LogTestV2, Unicode) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    std::pair<StringData, StringData> strs[] = {
        // Single byte characters that needs to be escaped
        {"\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd, "\\a\\b\\f\\n\\r\\t\\v\\\\\\0\\x7f\\e"_sd},
        // multi byte characters that needs to be escaped (unicode control characters)
        {"\u0080\u009f"_sd, "\\xc2\\x80\\xc2\\x9f"_sd},
        // Valid 2 Octet sequence, LATIN SMALL LETTER N WITH TILDE
        {"\u00f1"_sd, "\u00f1"_sd},
        // Invalid 2 Octet Sequence, result is escaped
        {"\xc3\x28"_sd, "\\xc3\x28"_sd},
        // Invalid Sequence Identifier, result is escaped
        {"\xa0\xa1"_sd, "\\xa0\\xa1"_sd},
        // Valid 3 Octet sequence, RUNIC LETTER TIWAZ TIR TYR T
        {"\u16cf"_sd, "\u16cf"_sd},
        // Invalid 3 Octet Sequence (in 2nd Octet), result is escaped
        {"\xe2\x28\xa1"_sd, "\\xe2\x28\\xa1"_sd},
        // Invalid 3 Octet Sequence (in 3rd Octet), result is escaped
        {"\xe2\x82\x28"_sd, "\\xe2\\x82\x28"_sd},
        // Valid 4 Octet sequence, GOTHIC LETTER MANNA
        {"\U0001033c"_sd, "\U0001033c"_sd},
        // Invalid 4 Octet Sequence (in 2nd Octet), result is escaped
        {"\xf0\x28\x8c\xbc"_sd, "\\xf0\x28\\x8c\\xbc"_sd},
        // Invalid 4 Octet Sequence (in 3rd Octet), result is escaped
        {"\xf0\x90\x28\xbc"_sd, "\\xf0\\x90\x28\\xbc"_sd},
        // Invalid 4 Octet Sequence (in 4th Octet), result is escaped
        {"\xf0\x28\x8c\x28"_sd, "\\xf0\x28\\x8c\x28"_sd},
        // Valid 5 Octet Sequence (but not Unicode!), result is escaped
        {"\xf8\xa1\xa1\xa1\xa1"_sd, "\\xf8\\xa1\\xa1\\xa1\\xa1"_sd},
        // Valid 6 Octet Sequence (but not Unicode!), result is escaped
        {"\xfc\xa1\xa1\xa1\xa1\xa1"_sd, "\\xfc\\xa1\\xa1\\xa1\\xa1\\xa1"_sd},
        // Invalid 3 Octet sequence, buffer ends prematurely, result is escaped
        {"\xe2\x82"_sd, "\\xe2\\x82"_sd},
    };

    for (const auto& pair : strs) {
        LOGV2("{}", "name"_attr = pair.first);
        ASSERT_EQUALS(lines.back(), pair.second);
    }
}

TEST_F(LogTestV2, Threads) {
    std::vector<std::string> linesPlain;
    auto plainSink = LogTestBackend::create(linesPlain);
    plainSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    plainSink->set_formatter(PlainFormatter());
    attach(plainSink);

    std::vector<std::string> linesText;
    auto textSink = LogTestBackend::create(linesText);
    textSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    textSink->set_formatter(TextFormatter());
    attach(textSink);

    std::vector<std::string> linesJson;
    auto jsonSink = LogTestBackend::create(linesJson);
    jsonSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    jsonSink->set_formatter(JsonFormatter());
    attach(jsonSink);

    constexpr int kNumPerThread = 1000;
    std::vector<stdx::thread> threads;

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread1");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread2");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread3");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread4");
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
    auto testSink = LogTestBackend::create(lines);
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

    LOGV2("test");
    ASSERT(verifyRamLog());
    LOGV2("test2");
    ASSERT(verifyRamLog());
}

TEST_F(LogTestV2, MultipleDomains) {
    std::vector<std::string> global_lines;
    auto sink = LogTestBackend::create(global_lines);
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
    auto other_sink = LogTestBackend::create(other_lines);
    other_sink->set_filter(
        ComponentSettingsFilter(other_domain, LogManager::global().getGlobalSettings()));
    other_sink->set_formatter(PlainFormatter());
    attach(other_sink);

    LOGV2_OPTIONS({&other_domain}, "test");
    ASSERT(global_lines.empty());
    ASSERT(other_lines.back() == "test");

    LOGV2("global domain log");
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

    LOGV2("test");
    ASSERT(readFile(file_name).back() == "test");

    LOGV2("test2");
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

}  // namespace
}  // namespace logv2
}  // namespace mongo

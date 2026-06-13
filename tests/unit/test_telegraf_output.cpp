// ============================================================================
// ZeptoDB: Telegraf external output unit tests
// ============================================================================

#include "zeptodb/feeds/telegraf_output.h"

#include <gtest/gtest.h>

using namespace zeptodb::feeds;

TEST(TelegrafOutputTest, ParsesLineProtocolWithTagsFieldsAndTimestamp) {
    std::string error;
    auto metric = parse_telegraf_line(
        "cpu,symbol=AAPL,host=edge01 value=150.25,volume=10i 1711234567000000000",
        &error);

    ASSERT_TRUE(metric.has_value()) << error;
    EXPECT_EQ(metric->measurement, "cpu");
    EXPECT_EQ(metric->tags.at("symbol"), "AAPL");
    EXPECT_EQ(metric->tags.at("host"), "edge01");
    ASSERT_TRUE(metric->timestamp.has_value());
    EXPECT_EQ(*metric->timestamp, 1711234567000000000LL);
    ASSERT_TRUE(metric->fields.count("value"));
    EXPECT_EQ(metric->fields.at("value").type, TelegrafFieldValue::Type::Float);
    EXPECT_DOUBLE_EQ(metric->fields.at("value").f, 150.25);
    EXPECT_EQ(metric->fields.at("volume").type, TelegrafFieldValue::Type::Integer);
    EXPECT_EQ(metric->fields.at("volume").i, 10);
}

TEST(TelegrafOutputTest, HandlesEscapedMeasurementTagsAndQuotedStringFields) {
    std::string error;
    auto metric = parse_telegraf_line(
        R"(plant\,one,symbol=robot\ arm value=42i,status="needs \"service\"" 1000)",
        &error);

    ASSERT_TRUE(metric.has_value()) << error;
    EXPECT_EQ(metric->measurement, "plant,one");
    EXPECT_EQ(metric->tags.at("symbol"), "robot arm");
    EXPECT_EQ(metric->fields.at("status").type, TelegrafFieldValue::Type::String);
    EXPECT_EQ(metric->fields.at("status").s, R"(needs "service")");
}

TEST(TelegrafOutputTest, RejectsEmptyAndMalformedInput) {
    std::string error;
    EXPECT_FALSE(parse_telegraf_line("", &error).has_value());
    EXPECT_EQ(error, "empty line");

    EXPECT_FALSE(parse_telegraf_line("cpu value", &error).has_value());
    EXPECT_EQ(error, "invalid field assignment");

    EXPECT_FALSE(parse_telegraf_line("cpu value=nan", &error).has_value());
    EXPECT_EQ(error, "invalid field value");
}

TEST(TelegrafOutputTest, MapsMetricToSqlRowWithScales) {
    auto metric = parse_telegraf_line(
        "temp,symbol=BOILER_A value=12.345,volume=2.5 1711234567");
    ASSERT_TRUE(metric.has_value());

    TelegrafOutputConfig cfg;
    cfg.price_scale = 1000.0;
    cfg.volume_scale = 10.0;
    cfg.timestamp_unit = TelegrafTimestampUnit::Seconds;

    std::string error;
    auto row = metric_to_telegraf_sql_row(*metric, cfg, &error);

    ASSERT_TRUE(row.has_value()) << error;
    EXPECT_EQ(row->symbol, "BOILER_A");
    EXPECT_EQ(row->price, 12345);
    EXPECT_EQ(row->volume, 25);
    EXPECT_EQ(row->timestamp_ns, 1711234567000000000LL);
}

TEST(TelegrafOutputTest, FallsBackToMeasurementAsSymbolAndDefaultVolume) {
    auto metric = parse_telegraf_line("cpu value=7i 42");
    ASSERT_TRUE(metric.has_value());

    TelegrafOutputConfig cfg;
    cfg.default_volume = 3;

    std::string error;
    auto row = metric_to_telegraf_sql_row(*metric, cfg, &error);

    ASSERT_TRUE(row.has_value()) << error;
    EXPECT_EQ(row->symbol, "cpu");
    EXPECT_EQ(row->price, 7);
    EXPECT_EQ(row->volume, 3);
    EXPECT_EQ(row->timestamp_ns, 42);
}

TEST(TelegrafOutputTest, RejectsMissingPriceAndNonNumericPrice) {
    TelegrafOutputConfig cfg;
    std::string error;

    auto missing = parse_telegraf_line("cpu,symbol=A value=1i 42");
    ASSERT_TRUE(missing.has_value());
    cfg.price_field = "load";
    EXPECT_FALSE(metric_to_telegraf_sql_row(*missing, cfg, &error).has_value());
    EXPECT_EQ(error, "price field 'load' is missing");

    cfg.price_field = "value";
    auto non_numeric = parse_telegraf_line(R"(cpu,symbol=A value="hot" 42)");
    ASSERT_TRUE(non_numeric.has_value());
    EXPECT_FALSE(metric_to_telegraf_sql_row(*non_numeric, cfg, &error).has_value());
    EXPECT_EQ(error, "price field is not numeric");
}

TEST(TelegrafOutputTest, BuildsEscapedSqlBatch) {
    TelegrafOutputConfig cfg;
    cfg.table_name = "iot_ticks";
    std::vector<TelegrafSqlRow> rows{
        {"O'HARE", 100, 1, 10},
        {"AAPL", 200, 2, 20},
    };

    auto built = build_telegraf_insert_sql(rows, cfg);

    ASSERT_TRUE(built.errors.empty());
    EXPECT_EQ(built.rows, 2u);
    EXPECT_EQ(
        built.sql,
        "INSERT INTO iot_ticks (symbol, price, volume, timestamp) VALUES "
        "('O''HARE', 100, 1, 10), ('AAPL', 200, 2, 20)");
}

TEST(TelegrafOutputTest, RejectsUnsafeTableName) {
    TelegrafOutputConfig cfg;
    cfg.table_name = "ticks;DROP";
    std::vector<TelegrafSqlRow> rows{{"AAPL", 1, 1, 1}};

    auto built = build_telegraf_insert_sql(rows, cfg);

    EXPECT_FALSE(built.errors.empty());
    EXPECT_TRUE(built.sql.empty());
}

TEST(TelegrafOutputTest, ParsesTimestampUnits) {
    EXPECT_EQ(parse_telegraf_timestamp_unit("ns"), TelegrafTimestampUnit::Nanoseconds);
    EXPECT_EQ(parse_telegraf_timestamp_unit("us"), TelegrafTimestampUnit::Microseconds);
    EXPECT_EQ(parse_telegraf_timestamp_unit("ms"), TelegrafTimestampUnit::Milliseconds);
    EXPECT_EQ(parse_telegraf_timestamp_unit("s"), TelegrafTimestampUnit::Seconds);
    EXPECT_FALSE(parse_telegraf_timestamp_unit("minutes").has_value());
}

TEST(TelegrafOutputTest, RejectsTimestampScaleOverflow) {
    auto metric = parse_telegraf_line("cpu,symbol=A value=1i 9223372036854775807");
    ASSERT_TRUE(metric.has_value());

    TelegrafOutputConfig cfg;
    cfg.timestamp_unit = TelegrafTimestampUnit::Seconds;

    std::string error;
    EXPECT_FALSE(metric_to_telegraf_sql_row(*metric, cfg, &error).has_value());
    EXPECT_EQ(error, "timestamp conversion is outside int64 ns range");
}

// ============================================================================
// APEX-DB: KafkaConsumer unit tests
// ============================================================================
// Tests exercise decode / routing logic without a live Kafka broker.
// Actual broker connectivity is tested via integration tests only.
// ============================================================================

#include "apex/feeds/kafka_consumer.h"
#include "apex/core/pipeline.h"

#include <gtest/gtest.h>
#include <cstring>

using namespace apex::feeds;
using apex::ingestion::TickMessage;

// ============================================================================
// KafkaConfig
// ============================================================================

TEST(KafkaConsumerTest, ConfigDefaults) {
    KafkaConfig cfg;
    cfg.topic = "market_data";

    EXPECT_EQ(cfg.brokers, "localhost:9092");
    EXPECT_EQ(cfg.group_id, "apex-consumer");
    EXPECT_EQ(cfg.auto_offset_reset, "latest");
    EXPECT_EQ(cfg.poll_timeout_ms, 100);
    EXPECT_EQ(cfg.format, MessageFormat::JSON);
    EXPECT_DOUBLE_EQ(cfg.price_scale, 10000.0);
}

// ============================================================================
// decode_json
// ============================================================================

TEST(KafkaConsumerTest, DecodeJsonBasic) {
    const char* json = R"({"symbol_id":1,"price":15000,"volume":100,"ts":123456789})";
    auto result = KafkaConsumer::decode_json(json, std::strlen(json));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 1u);
    EXPECT_EQ(result->price,     15000);
    EXPECT_EQ(result->volume,    100u);
    EXPECT_EQ(result->recv_ts,   123456789);
    EXPECT_EQ(result->msg_type,  0);
}

TEST(KafkaConsumerTest, DecodeJsonTsOptional) {
    // ts field absent — should still decode, recv_ts = 0
    const char* json = R"({"symbol_id":2,"price":20000,"volume":50})";
    auto result = KafkaConsumer::decode_json(json, std::strlen(json));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 2u);
    EXPECT_EQ(result->recv_ts,   0);
}

TEST(KafkaConsumerTest, DecodeJsonMissingSymbolId) {
    const char* json = R"({"price":15000,"volume":100})";
    auto result = KafkaConsumer::decode_json(json, std::strlen(json));
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeJsonMissingPrice) {
    const char* json = R"({"symbol_id":1,"volume":100})";
    auto result = KafkaConsumer::decode_json(json, std::strlen(json));
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeJsonInvalid) {
    const char* json = "not_json_at_all";
    auto result = KafkaConsumer::decode_json(json, std::strlen(json));
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeJsonNullOrEmpty) {
    EXPECT_FALSE(KafkaConsumer::decode_json(nullptr, 0).has_value());
    EXPECT_FALSE(KafkaConsumer::decode_json("", 0).has_value());
}

// ============================================================================
// decode_binary
// ============================================================================

TEST(KafkaConsumerTest, DecodeBinaryExact) {
    TickMessage original{};
    original.symbol_id = 42;
    original.price     = 99000;
    original.volume    = 200;
    original.recv_ts   = 9876543210LL;
    original.msg_type  = 1;

    auto result = KafkaConsumer::decode_binary(
        reinterpret_cast<const char*>(&original), sizeof(original));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 42u);
    EXPECT_EQ(result->price,     99000);
    EXPECT_EQ(result->volume,    200u);
    EXPECT_EQ(result->recv_ts,   9876543210LL);
    EXPECT_EQ(result->msg_type,  1);
}

TEST(KafkaConsumerTest, DecodeBinaryTooShort) {
    TickMessage msg{};
    // one byte short
    auto result = KafkaConsumer::decode_binary(
        reinterpret_cast<const char*>(&msg), sizeof(msg) - 1);
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeBinaryNull) {
    EXPECT_FALSE(KafkaConsumer::decode_binary(nullptr, 64).has_value());
}

// ============================================================================
// decode_json_human
// ============================================================================

TEST(KafkaConsumerTest, DecodeJsonHumanKnownSymbol) {
    std::unordered_map<std::string, apex::SymbolId> sym_map{{"AAPL", 1}, {"MSFT", 2}};

    const char* json = R"({"symbol":"AAPL","price":150.50,"volume":300,"ts":111})";
    auto result = KafkaConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 10000.0);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 1u);
    EXPECT_EQ(result->price,     1505000);  // 150.50 * 10000
    EXPECT_EQ(result->volume,    300u);
    EXPECT_EQ(result->recv_ts,   111);
}

TEST(KafkaConsumerTest, DecodeJsonHumanUnknownSymbol) {
    std::unordered_map<std::string, apex::SymbolId> sym_map{{"AAPL", 1}};
    const char* json = R"({"symbol":"TSLA","price":200.0,"volume":50})";
    auto result = KafkaConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 10000.0);
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeJsonHumanMissingPrice) {
    std::unordered_map<std::string, apex::SymbolId> sym_map{{"AAPL", 1}};
    const char* json = R"({"symbol":"AAPL","volume":50})";
    auto result = KafkaConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 10000.0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Routing / ingest_decoded
// ============================================================================

TEST(KafkaConsumerTest, IngestDecodedNoPipeline) {
    KafkaConfig cfg;
    cfg.topic = "test";
    KafkaConsumer consumer(cfg);
    // No pipeline or router set

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 10000;
    msg.volume    = 1;

    EXPECT_FALSE(consumer.ingest_decoded(msg));
}

TEST(KafkaConsumerTest, IngestDecodedSingleNode) {
    KafkaConfig cfg;
    cfg.topic = "test";
    KafkaConsumer consumer(cfg);

    apex::core::ApexPipeline pipeline;
    consumer.set_pipeline(&pipeline);
    // Pipeline not started — ingest_tick() still enqueues into ring buffer.

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 15000;
    msg.volume    = 100;

    bool ok = consumer.ingest_decoded(msg);
    EXPECT_TRUE(ok);

    KafkaStats s = consumer.stats();
    EXPECT_EQ(s.route_local,     1u);
    EXPECT_EQ(s.route_remote,    0u);
    EXPECT_EQ(s.ingest_failures, 0u);
}

// ============================================================================
// on_message: stats tracking
// ============================================================================

TEST(KafkaConsumerTest, OnMessageUpdatesStats) {
    KafkaConfig cfg;
    cfg.topic  = "test";
    cfg.format = MessageFormat::JSON;
    KafkaConsumer consumer(cfg);

    apex::core::ApexPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    const char* valid = R"({"symbol_id":1,"price":15000,"volume":100})";
    consumer.on_message(valid, std::strlen(valid));

    KafkaStats s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 1u);
    EXPECT_EQ(s.decode_errors,     0u);
    EXPECT_GT(s.bytes_consumed,    0u);
}

TEST(KafkaConsumerTest, OnMessageDecodeErrorIncrementsStats) {
    KafkaConfig cfg;
    cfg.topic  = "test";
    cfg.format = MessageFormat::JSON;
    KafkaConsumer consumer(cfg);

    const char* garbage = "{{not valid json}}";
    consumer.on_message(garbage, std::strlen(garbage));

    KafkaStats s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 0u);
    EXPECT_EQ(s.decode_errors,     1u);
}

// ============================================================================
// CommitMode and Backpressure config
// ============================================================================

TEST(KafkaConsumerTest, CommitModeDefaultIsAfterIngest) {
    KafkaConfig cfg;
    EXPECT_EQ(cfg.commit_mode, CommitMode::AFTER_INGEST);
}

TEST(KafkaConsumerTest, BackpressureDefaultConfig) {
    KafkaConfig cfg;
    EXPECT_EQ(cfg.backpressure_retries,  3);
    EXPECT_EQ(cfg.backpressure_sleep_us, 100);
}

TEST(KafkaConsumerTest, BackpressureZeroRetriesFailsImmediately) {
    KafkaConfig cfg;
    cfg.topic                 = "test";
    cfg.backpressure_retries  = 0;
    cfg.backpressure_sleep_us = 0;
    KafkaConsumer consumer(cfg);
    // No pipeline — ingest_decoded must fail on first attempt without retrying.
    TickMessage msg{};
    msg.symbol_id = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 0u);  // no pipeline → returns false, not ingest_failure
}

TEST(KafkaConsumerTest, BackpressureSucceedsAfterRetry) {
    // Configure a consumer with retries, attach a real (not-started) pipeline.
    // ingest_tick() on a non-started pipeline still enqueues to the ring buffer,
    // so the first call already succeeds — verifies the retry path doesn't break
    // the happy path.
    KafkaConfig cfg;
    cfg.topic                 = "test";
    cfg.backpressure_retries  = 3;
    cfg.backpressure_sleep_us = 0;  // no sleep in tests
    KafkaConsumer consumer(cfg);

    apex::core::ApexPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 15000;
    msg.volume    = 100;
    EXPECT_TRUE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().route_local,     1u);
    EXPECT_EQ(consumer.stats().ingest_failures, 0u);
}

// ============================================================================
// format_prometheus() — Prometheus/OpenMetrics text output
// ============================================================================

TEST(KafkaConsumerTest, FormatPrometheusCounters) {
    KafkaStats s;
    s.messages_consumed = 1000;
    s.bytes_consumed    = 64000;
    s.decode_errors     = 3;
    s.route_local       = 900;
    s.route_remote      = 100;
    s.ingest_failures   = 2;

    const std::string out = KafkaConsumer::format_prometheus("trades", s);

    // Counter names must be present with correct label + value
    EXPECT_NE(out.find("apex_kafka_messages_consumed_total{consumer=\"trades\"} 1000"), std::string::npos);
    EXPECT_NE(out.find("apex_kafka_bytes_consumed_total{consumer=\"trades\"} 64000"), std::string::npos);
    EXPECT_NE(out.find("apex_kafka_decode_errors_total{consumer=\"trades\"} 3"), std::string::npos);
    EXPECT_NE(out.find("apex_kafka_route_local_total{consumer=\"trades\"} 900"), std::string::npos);
    EXPECT_NE(out.find("apex_kafka_route_remote_total{consumer=\"trades\"} 100"), std::string::npos);
    EXPECT_NE(out.find("apex_kafka_ingest_failures_total{consumer=\"trades\"} 2"), std::string::npos);
}

TEST(KafkaConsumerTest, FormatPrometheusIncludesHelp) {
    KafkaStats s{};
    const std::string out = KafkaConsumer::format_prometheus("quotes", s);

    EXPECT_NE(out.find("# HELP apex_kafka_messages_consumed_total"), std::string::npos);
    EXPECT_NE(out.find("# TYPE apex_kafka_messages_consumed_total counter"), std::string::npos);
    EXPECT_NE(out.find("# HELP apex_kafka_ingest_failures_total"), std::string::npos);
}

TEST(KafkaConsumerTest, FormatPrometheusZeroStats) {
    KafkaStats s{};
    const std::string out = KafkaConsumer::format_prometheus("empty", s);

    EXPECT_NE(out.find("{consumer=\"empty\"} 0"), std::string::npos);
    // All six counters should be 0
    size_t pos = 0;
    int zero_lines = 0;
    while ((pos = out.find("} 0", pos)) != std::string::npos) {
        ++zero_lines;
        ++pos;
    }
    EXPECT_EQ(zero_lines, 6);
}

TEST(KafkaConsumerTest, FormatPrometheusLabelEscaping) {
    // Label value with special chars — must appear verbatim (simple name here)
    KafkaStats s{};
    s.messages_consumed = 42;
    const std::string out = KafkaConsumer::format_prometheus("my-topic-v2", s);
    EXPECT_NE(out.find("{consumer=\"my-topic-v2\"} 42"), std::string::npos);
}

// ============================================================================
// start() without Kafka compiled in
// ============================================================================

TEST(KafkaConsumerTest, StartWithoutKafkaLibrary) {
    KafkaConfig cfg;
    cfg.brokers = "localhost:9092";
    cfg.topic   = "test";
    KafkaConsumer consumer(cfg);

#ifdef APEX_KAFKA_AVAILABLE
    // When Kafka is available, start() may return true or false depending on
    // whether a broker is reachable.  We just check it doesn't crash.
    (void)consumer.start();
    consumer.stop();
#else
    // Without Kafka, start() must return false gracefully.
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
#endif
}

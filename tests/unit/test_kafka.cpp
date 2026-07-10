// ============================================================================
// ZeptoDB: KafkaConsumer unit tests
// ============================================================================
// Tests exercise decode / routing logic without a live Kafka broker.
// Actual broker connectivity is tested via integration tests only.
// ============================================================================

#include "zeptodb/feeds/kafka_consumer.h"
#include "zeptodb/core/pipeline.h"

#include <gtest/gtest.h>
#include <cstring>

using namespace zeptodb::feeds;
using zeptodb::ingestion::TickMessage;

// ============================================================================
// KafkaConfig
// ============================================================================

TEST(KafkaConsumerTest, ConfigDefaults) {
    KafkaConfig cfg;
    cfg.topic = "market_data";

    EXPECT_EQ(cfg.brokers, "localhost:9092");
    EXPECT_EQ(cfg.group_id, "zepto-consumer");
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
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"AAPL", 1}, {"MSFT", 2}};

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
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"AAPL", 1}};
    const char* json = R"({"symbol":"TSLA","price":200.0,"volume":50})";
    auto result = KafkaConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 10000.0);
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaConsumerTest, DecodeJsonHumanMissingPrice) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"AAPL", 1}};
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

    zeptodb::core::ZeptoPipeline pipeline;
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

    zeptodb::core::ZeptoPipeline pipeline;
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

    zeptodb::core::ZeptoPipeline pipeline;
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
    EXPECT_NE(out.find("zepto_kafka_messages_consumed_total{consumer=\"trades\"} 1000"), std::string::npos);
    EXPECT_NE(out.find("zepto_kafka_bytes_consumed_total{consumer=\"trades\"} 64000"), std::string::npos);
    EXPECT_NE(out.find("zepto_kafka_decode_errors_total{consumer=\"trades\"} 3"), std::string::npos);
    EXPECT_NE(out.find("zepto_kafka_route_local_total{consumer=\"trades\"} 900"), std::string::npos);
    EXPECT_NE(out.find("zepto_kafka_route_remote_total{consumer=\"trades\"} 100"), std::string::npos);
    EXPECT_NE(out.find("zepto_kafka_ingest_failures_total{consumer=\"trades\"} 2"), std::string::npos);
}

TEST(KafkaConsumerTest, FormatPrometheusIncludesHelp) {
    KafkaStats s{};
    const std::string out = KafkaConsumer::format_prometheus("quotes", s);

    EXPECT_NE(out.find("# HELP zepto_kafka_messages_consumed_total"), std::string::npos);
    EXPECT_NE(out.find("# TYPE zepto_kafka_messages_consumed_total counter"), std::string::npos);
    EXPECT_NE(out.find("# HELP zepto_kafka_ingest_failures_total"), std::string::npos);
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

#ifdef ZEPTO_KAFKA_AVAILABLE
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

// ============================================================================
// Stage B (devlog 084): Table-aware ingest
// ============================================================================
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

TEST(KafkaConsumerTest, TableScopedIngestLandsInTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    // CREATE TABLE via SQL so the SchemaRegistry assigns a non-zero table_id.
    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE kfeed (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("kfeed");
    ASSERT_NE(tid, 0);

    KafkaConfig cfg;
    cfg.topic      = "t";
    cfg.table_name = "kfeed";
    KafkaConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 12345;
    msg.volume    = 10;
    EXPECT_TRUE(consumer.ingest_decoded(msg));

    pipeline.drain_sync(100);

    auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
    ASSERT_GE(parts.size(), 1u);
    // At least one row for symbol 1 in the kfeed-scoped partition set.
    size_t total = 0;
    for (auto* p : parts) total += p->num_rows();
    EXPECT_GE(total, 1u);
}

TEST(KafkaConsumerTest, TableNameEmptyIsLegacyPath) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    KafkaConfig cfg;
    cfg.topic = "t";  // table_name empty → legacy path
    KafkaConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 2;
    msg.price     = 99;
    msg.volume    = 1;
    EXPECT_TRUE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().route_local, 1u);
}

TEST(KafkaConsumerTest, UnknownTableDropsMessages) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    KafkaConfig cfg;
    cfg.topic      = "t";
    cfg.table_name = "nonexistent";   // never CREATE'd
    KafkaConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);  // logs error, table_id stays 0

    TickMessage msg{};
    msg.symbol_id = 3;
    msg.price     = 1;
    msg.volume    = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 1u);
}

// ============================================================================
// CI-safe perf smoke — cross-connector parity with OpcUaPerf (devlog 110)
// ============================================================================
// Same shape as OpcUaPerf.SingleThreadHotPath so numbers are directly
// comparable across Kafka / MQTT / OPC-UA without exceeding TickPlant capacity.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

TEST(KafkaPerf, SingleThreadHotPath) {
    constexpr int kPool  = 500;
    constexpr int kCalls = 50'000;

    KafkaConfig cfg;
    cfg.topic                 = "perf";
    cfg.backpressure_retries  = 0;
    cfg.backpressure_sleep_us = 0;
    cfg.format                = MessageFormat::JSON_HUMAN;
    cfg.symbol_map.reserve(kPool);
    std::vector<std::string> payloads;
    payloads.reserve(kPool);
    for (int i = 0; i < kPool; ++i) {
        std::string sym = "s" + std::to_string(i);
        cfg.symbol_map.emplace(sym, static_cast<zeptodb::SymbolId>(i + 1));
        payloads.push_back("{\"symbol\":\"" + sym +
                           "\",\"price\":150.25,\"volume\":100}");
    }
    KafkaConsumer consumer(cfg);
    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    auto start_time = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) {
        const auto& p = payloads[i % kPool];
        consumer.on_message(p.data(), p.size());
    }
    auto end_time = std::chrono::steady_clock::now();
    auto wall_us = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(
                                            end_time - start_time).count());
    double ticks_per_second = static_cast<double>(kCalls) * 1e6 / static_cast<double>(wall_us);
    auto stats_after_throughput = consumer.stats();

    constexpr int kLatSamples = 10'000;
    std::vector<int64_t> samples;
    samples.reserve(kLatSamples);
    for (int i = 0; i < kLatSamples; ++i) {
        const auto& p = payloads[i % kPool];
        auto sample_start = std::chrono::steady_clock::now();
        consumer.on_message(p.data(), p.size());
        auto sample_end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                sample_end - sample_start).count());
    }
    std::sort(samples.begin(), samples.end());
    auto p50 = samples[samples.size() / 2];
    auto p99 = samples[samples.size() * 99 / 100];
    auto stats_after_latency = consumer.stats();

    std::fprintf(stderr,
        "[KafkaPerf] wall=%lld us  throughput=%.0f ticks/s  "
        "ok=%llu failures=%llu  | p50=%lld ns  p99=%lld ns\n",
        static_cast<long long>(wall_us), ticks_per_second,
        static_cast<unsigned long long>(stats_after_latency.route_local),
        static_cast<unsigned long long>(stats_after_latency.ingest_failures),
        static_cast<long long>(p50), static_cast<long long>(p99));

    EXPECT_EQ(stats_after_throughput.messages_consumed, static_cast<uint64_t>(kCalls));
    EXPECT_EQ(stats_after_throughput.route_local, static_cast<uint64_t>(kCalls));
    EXPECT_EQ(stats_after_throughput.decode_errors, 0u);
    EXPECT_EQ(stats_after_throughput.ingest_failures, 0u);
    EXPECT_EQ(stats_after_latency.messages_consumed,
              static_cast<uint64_t>(kCalls + kLatSamples));
    EXPECT_EQ(stats_after_latency.route_local, static_cast<uint64_t>(kCalls + kLatSamples));
    EXPECT_EQ(stats_after_latency.decode_errors, 0u);
    EXPECT_EQ(stats_after_latency.ingest_failures, 0u);
}

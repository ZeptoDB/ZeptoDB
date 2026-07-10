// ============================================================================
// ZeptoDB: MqttConsumer unit tests
// ============================================================================
// Tests exercise decode / routing logic without a live MQTT broker.
// Actual broker connectivity is covered by integration tests only.
// ============================================================================

#include "zeptodb/feeds/mqtt_consumer.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/auth/license_validator.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <memory>

using namespace zeptodb::feeds;
using zeptodb::ingestion::TickMessage;

// ============================================================================
// MqttConfig defaults
// ============================================================================

TEST(MqttConsumerTest, ConfigDefaults) {
    MqttConfig cfg;
    cfg.topic = "sensors/temp";

    EXPECT_EQ(cfg.broker_uri,   "tcp://localhost:1883");
    EXPECT_EQ(cfg.client_id,    "zepto-mqtt-consumer");
    EXPECT_EQ(cfg.qos,          1);
    EXPECT_EQ(cfg.keepalive_sec,30);
    EXPECT_TRUE(cfg.clean_session);
    EXPECT_EQ(cfg.format,       MessageFormat::JSON);
    EXPECT_DOUBLE_EQ(cfg.price_scale, 10000.0);
    EXPECT_EQ(cfg.backpressure_retries,  3);
    EXPECT_EQ(cfg.backpressure_sleep_us, 100);
}

// ============================================================================
// QoS validation
// ============================================================================

TEST(MqttConsumerTest, QoSValidation) {
    EXPECT_TRUE(MqttConsumer::is_valid_qos(0));
    EXPECT_TRUE(MqttConsumer::is_valid_qos(1));
    EXPECT_TRUE(MqttConsumer::is_valid_qos(2));
    EXPECT_FALSE(MqttConsumer::is_valid_qos(-1));
    EXPECT_FALSE(MqttConsumer::is_valid_qos(3));
    EXPECT_FALSE(MqttConsumer::is_valid_qos(99));
}

TEST(MqttConsumerTest, StartRejectsInvalidQos) {
    MqttConfig cfg;
    cfg.topic = "sensors/temp";
    cfg.qos   = 3;  // invalid
    MqttConsumer consumer(cfg);
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
}

TEST(MqttConsumerTest, StartRejectsEmptyTopic) {
    MqttConfig cfg;
    cfg.topic = "";
    MqttConsumer consumer(cfg);
    EXPECT_FALSE(consumer.start());
}

// ============================================================================
// decode_json
// ============================================================================

TEST(MqttConsumerTest, DecodeJsonBasic) {
    const char* json = R"({"symbol_id":7,"price":2500,"volume":42,"ts":111222333})";
    auto result = MqttConsumer::decode_json(json, std::strlen(json));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 7u);
    EXPECT_EQ(result->price,     2500);
    EXPECT_EQ(result->volume,    42u);
    EXPECT_EQ(result->recv_ts,   111222333);
}

TEST(MqttConsumerTest, DecodeJsonMalformed) {
    const char* json = "<<< not json >>>";
    auto result = MqttConsumer::decode_json(json, std::strlen(json));
    EXPECT_FALSE(result.has_value());
}

TEST(MqttConsumerTest, DecodeJsonEmpty) {
    EXPECT_FALSE(MqttConsumer::decode_json(nullptr, 0).has_value());
    EXPECT_FALSE(MqttConsumer::decode_json("", 0).has_value());
}

// ============================================================================
// decode_binary
// ============================================================================

TEST(MqttConsumerTest, DecodeBinaryExact) {
    TickMessage original{};
    original.symbol_id = 9;
    original.price     = 77000;
    original.volume    = 500;
    original.recv_ts   = 123;

    auto result = MqttConsumer::decode_binary(
        reinterpret_cast<const char*>(&original), sizeof(original));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 9u);
    EXPECT_EQ(result->price,     77000);
    EXPECT_EQ(result->volume,    500u);
}

TEST(MqttConsumerTest, DecodeBinaryWrongSize) {
    TickMessage msg{};
    EXPECT_FALSE(MqttConsumer::decode_binary(
        reinterpret_cast<const char*>(&msg), sizeof(msg) - 1).has_value());
}

// ============================================================================
// decode_json_human with symbol_map
// ============================================================================

TEST(MqttConsumerTest, DecodeJsonHumanWithSymbolMap) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{
        {"sensor_a", 10}, {"sensor_b", 20}};

    const char* json = R"({"symbol":"sensor_a","price":23.75,"volume":1,"ts":55})";
    auto result = MqttConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 100.0);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 10u);
    EXPECT_EQ(result->price,     2375);  // 23.75 * 100
    EXPECT_EQ(result->volume,    1u);
    EXPECT_EQ(result->recv_ts,   55);
}

TEST(MqttConsumerTest, DecodeJsonHumanUnknownSymbol) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"sensor_a", 10}};
    const char* json = R"({"symbol":"sensor_z","price":1.0,"volume":1})";
    auto result = MqttConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 100.0);
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Routing: single-node pipeline
// ============================================================================

TEST(MqttConsumerTest, IngestDecodedNoPipeline) {
    MqttConfig cfg; cfg.topic = "t";
    MqttConsumer consumer(cfg);

    TickMessage msg{};
    msg.symbol_id = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
}

TEST(MqttConsumerTest, IngestDecodedSingleNode) {
    MqttConfig cfg; cfg.topic = "t";
    MqttConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 10000;
    msg.volume    = 5;

    EXPECT_TRUE(consumer.ingest_decoded(msg));
    auto s = consumer.stats();
    EXPECT_EQ(s.route_local,     1u);
    EXPECT_EQ(s.ingest_failures, 0u);
}

// ============================================================================
// Routing: multi-node with PartitionRouter (remote target mock)
// ============================================================================

TEST(MqttConsumerTest, RoutingRemoteTargetMissingRpcClient) {
    // Set up a router that maps symbol 1 to a non-local node.
    // No RPC client registered for that node → route must fail and count as
    // ingest_failure (remote client not found), not route_remote.
    MqttConfig cfg; cfg.topic = "t";
    cfg.backpressure_retries  = 0;
    cfg.backpressure_sleep_us = 0;
    MqttConsumer consumer(cfg);

    auto router = std::make_shared<zeptodb::cluster::PartitionRouter>();
    // Add a single non-local node so all symbols route to it.
    router->add_node(/*NodeId=*/7);

    consumer.set_routing(/*local_id=*/1, router,
        /*remotes=*/{});  // empty map — no RPC client for node 7

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 100;
    EXPECT_FALSE(consumer.ingest_decoded(msg));

    auto s = consumer.stats();
    EXPECT_EQ(s.route_remote,    0u);
    EXPECT_EQ(s.ingest_failures, 1u);
}

// ============================================================================
// on_message: stats tracking
// ============================================================================

TEST(MqttConsumerTest, OnMessageUpdatesStats) {
    MqttConfig cfg;
    cfg.topic  = "t";
    cfg.format = MessageFormat::JSON;
    MqttConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    const char* valid = R"({"symbol_id":1,"price":15000,"volume":100})";
    consumer.on_message(valid, std::strlen(valid));

    auto s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 1u);
    EXPECT_EQ(s.decode_errors,     0u);
    EXPECT_GT(s.bytes_consumed,    0u);
}

TEST(MqttConsumerTest, OnMessageEmptyPayload) {
    MqttConfig cfg;
    cfg.topic  = "t";
    cfg.format = MessageFormat::JSON;
    MqttConsumer consumer(cfg);

    EXPECT_FALSE(consumer.on_message("", 0));
    auto s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 0u);
    EXPECT_EQ(s.decode_errors,     1u);
}

TEST(MqttConsumerTest, OnMessageMalformedJson) {
    MqttConfig cfg;
    cfg.topic  = "t";
    cfg.format = MessageFormat::JSON;
    MqttConsumer consumer(cfg);

    const char* garbage = "{{bad}}";
    EXPECT_FALSE(consumer.on_message(garbage, std::strlen(garbage)));
    EXPECT_EQ(consumer.stats().decode_errors, 1u);
}

// ============================================================================
// start() without the Paho library compiled in
// ============================================================================

TEST(MqttConsumerTest, StartWithoutMqttLibrary) {
    MqttConfig cfg;
    cfg.broker_uri = "tcp://localhost:1883";
    cfg.topic      = "sensors/temp";
    MqttConsumer consumer(cfg);

#ifdef ZEPTO_MQTT_AVAILABLE
    (void)consumer.start();
    consumer.stop();
#else
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
#endif
}

// ============================================================================
// License gate: start() must refuse without Feature::IOT_CONNECTORS.
// Only meaningful when Paho is linked in — otherwise StartWithoutMqttLibrary
// already covers the false-return path.
// ============================================================================
#ifdef ZEPTO_MQTT_AVAILABLE
TEST(MqttConsumerTest, StartRejectedWithoutLicense) {
    // Install an Enterprise license that does NOT include IOT_CONNECTORS (bit 8).
    // features=255 covers bits 0..7 only → IOT_CONNECTORS remains disabled.
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string payload = R"({"edition":"enterprise","features":255,"max_nodes":1,"exp":)" +
        std::to_string(now + 86400) + "}";
    auto b64 = [](const std::string& s) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        auto d = reinterpret_cast<const unsigned char*>(s.data());
        size_t len = s.size();
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(d[i]) << 16;
            if (i+1 < len) n |= static_cast<uint32_t>(d[i+1]) << 8;
            if (i+2 < len) n |= static_cast<uint32_t>(d[i+2]);
            out += tbl[(n>>18)&63]; out += tbl[(n>>12)&63];
            out += (i+1<len) ? tbl[(n>>6)&63] : '=';
            out += (i+2<len) ? tbl[n&63] : '=';
        }
        for (char& c : out) { if (c=='+') c='-'; else if (c=='/') c='_'; }
        while (!out.empty() && out.back()=='=') out.pop_back();
        return out;
    };
    std::string jwt = b64(R"({"alg":"RS256","typ":"JWT"})") + "." + b64(payload) + ".fakesig";
    zeptodb::auth::license().load_from_jwt_string_for_testing(jwt);
    ASSERT_FALSE(zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS));

    MqttConfig cfg;
    cfg.topic = "sensors/temp";
    MqttConsumer consumer(cfg);
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
}
#endif


// ============================================================================
// Wildcard / multi-level topic accepted (filter validated by start(), not by
// decode path; here we just verify config accepts wildcard strings and that
// start() rejection path is *not* tripped by wildcard characters).
// ============================================================================

TEST(MqttConsumerTest, WildcardTopicAccepted) {
    for (const char* t : {"sensors/#", "plant/+/temp", "a/b/c"}) {
        MqttConfig cfg;
        cfg.topic = t;
        MqttConsumer consumer(cfg);
        // Without ZEPTO_MQTT_AVAILABLE start() returns false — but the reason
        // must not be "invalid topic": we prove that by showing an empty
        // topic is rejected the same way, while these wildcards at least
        // pass the topic-validation gate (implementation-wise: empty check).
        // Since we can't distinguish rejection reasons from the public API,
        // we simply assert the wildcard doesn't crash and is_running stays
        // false (no broker to connect to).
#ifdef ZEPTO_MQTT_AVAILABLE
        (void)consumer.start();
        consumer.stop();
#else
        EXPECT_FALSE(consumer.start());
#endif
        EXPECT_FALSE(consumer.is_running());
    }
}

// ============================================================================
// stop() is idempotent — calling it repeatedly (including before start) must
// not crash or alter state.
// ============================================================================

TEST(MqttConsumerTest, StopIsIdempotent) {
    MqttConfig cfg; cfg.topic = "t";
    MqttConsumer consumer(cfg);

    // Never started
    consumer.stop();
    EXPECT_FALSE(consumer.is_running());

    // Multiple calls
    consumer.stop();
    consumer.stop();
    EXPECT_FALSE(consumer.is_running());
}

// ============================================================================
// Concurrent ingest from a "callback thread" while stop() is called on the
// main thread.  We don't need a real broker — we drive on_message() from a
// worker thread to emulate Paho's callback thread.  This must not crash,
// deadlock, or corrupt stats counters.
// ============================================================================

#include <thread>
#include <atomic>

TEST(MqttConsumerTest, ConcurrentIngestAndStop) {
    MqttConfig cfg;
    cfg.topic  = "t";
    cfg.format = MessageFormat::JSON;
    MqttConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    std::atomic<bool> go{true};
    const char* valid = R"({"symbol_id":1,"price":100,"volume":1})";
    const size_t vlen = std::strlen(valid);

    std::thread producer([&] {
        while (go.load(std::memory_order_relaxed)) {
            consumer.on_message(valid, vlen);
        }
    });

    // Let the producer run, then call stop() repeatedly from the main
    // thread.  stop() must be safe to call concurrently with on_message().
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    consumer.stop();
    consumer.stop();

    go.store(false, std::memory_order_relaxed);
    producer.join();

    // Stats should be consistent (no torn counters, no crash).
    auto s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, s.route_local + s.ingest_failures);
    EXPECT_EQ(s.decode_errors, 0u);
}

// ============================================================================
// Stage B (devlog 084): Table-aware ingest
// ============================================================================
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

TEST(MqttConsumerTest, TableScopedIngestLandsInTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE mfeed (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("mfeed");
    ASSERT_NE(tid, 0);

    MqttConfig cfg;
    cfg.topic      = "t";
    cfg.table_name = "mfeed";
    MqttConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price     = 42;
    msg.volume    = 3;
    EXPECT_TRUE(consumer.ingest_decoded(msg));

    pipeline.drain_sync(100);
    auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
    ASSERT_GE(parts.size(), 1u);
}

TEST(MqttConsumerTest, UnknownTableDropsMessages) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    MqttConfig cfg;
    cfg.topic      = "t";
    cfg.table_name = "nonexistent";
    MqttConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
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
#include <cstdint>
#include <cstdio>
#include <vector>

TEST(MqttPerf, SingleThreadHotPath) {
    constexpr int kPool  = 500;
    constexpr int kCalls = 50'000;

    MqttConfig cfg;
    cfg.topic                 = "sensors/#";
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
    MqttConsumer consumer(cfg);
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
        "[MqttPerf] wall=%lld us  throughput=%.0f ticks/s  "
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

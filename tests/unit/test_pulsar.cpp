// ============================================================================
// ZeptoDB: PulsarConsumer unit tests
// ============================================================================
// Tests exercise decode / routing logic without a live Pulsar broker.
// ============================================================================

#include "zeptodb/feeds/pulsar_consumer.h"

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

using namespace zeptodb::feeds;
using zeptodb::ingestion::TickMessage;

TEST(PulsarConsumerTest, ConfigDefaults) {
    PulsarConfig cfg;
    cfg.topic = "persistent://public/default/market-data";

    EXPECT_EQ(cfg.service_url, "pulsar://localhost:6650");
    EXPECT_EQ(cfg.subscription_name, "zepto-consumer");
    EXPECT_EQ(cfg.subscription_type, PulsarSubscriptionType::Shared);
    EXPECT_EQ(cfg.initial_position, PulsarInitialPosition::Latest);
    EXPECT_EQ(cfg.receive_timeout_ms, 100);
    EXPECT_EQ(cfg.max_messages_per_poll, 1024);
    EXPECT_EQ(cfg.receiver_queue_size, 1000);
    EXPECT_EQ(cfg.ack_grouping_time_ms, 100);
    EXPECT_EQ(cfg.negative_ack_redelivery_delay_ms, 60000);
    EXPECT_EQ(cfg.format, MessageFormat::JSON);
    EXPECT_DOUBLE_EQ(cfg.price_scale, 10000.0);
}

TEST(PulsarConsumerTest, SubscriptionTypeNames) {
    EXPECT_STREQ(PulsarConsumer::subscription_type_name(
        PulsarSubscriptionType::Exclusive), "exclusive");
    EXPECT_STREQ(PulsarConsumer::subscription_type_name(
        PulsarSubscriptionType::Shared), "shared");
    EXPECT_STREQ(PulsarConsumer::subscription_type_name(
        PulsarSubscriptionType::Failover), "failover");
    EXPECT_STREQ(PulsarConsumer::subscription_type_name(
        PulsarSubscriptionType::KeyShared), "key_shared");
}

TEST(PulsarConsumerTest, ConfigValidationHelpers) {
    EXPECT_TRUE(PulsarConsumer::is_valid_max_messages_per_poll(1));
    EXPECT_TRUE(PulsarConsumer::is_valid_max_messages_per_poll(100000));
    EXPECT_FALSE(PulsarConsumer::is_valid_max_messages_per_poll(0));
    EXPECT_FALSE(PulsarConsumer::is_valid_max_messages_per_poll(100001));

    EXPECT_TRUE(PulsarConsumer::is_valid_receiver_queue_size(1));
    EXPECT_FALSE(PulsarConsumer::is_valid_receiver_queue_size(0));
    EXPECT_FALSE(PulsarConsumer::is_valid_receiver_queue_size(-1));
}

TEST(PulsarConsumerTest, StartRejectsEmptyRequiredFields) {
    {
        PulsarConfig cfg;
        cfg.service_url = "";
        cfg.topic = "t";
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
        EXPECT_FALSE(consumer.is_running());
    }
    {
        PulsarConfig cfg;
        cfg.topic = "";
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
        EXPECT_FALSE(consumer.is_running());
    }
    {
        PulsarConfig cfg;
        cfg.topic = "t";
        cfg.subscription_name = "";
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
        EXPECT_FALSE(consumer.is_running());
    }
}

TEST(PulsarConsumerTest, StartRejectsInvalidBounds) {
    {
        PulsarConfig cfg;
        cfg.topic = "t";
        cfg.max_messages_per_poll = 0;
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
    }
    {
        PulsarConfig cfg;
        cfg.topic = "t";
        cfg.receiver_queue_size = 0;
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
    }
    {
        PulsarConfig cfg;
        cfg.topic = "t";
        cfg.receive_timeout_ms = 0;
        PulsarConsumer consumer(cfg);
        EXPECT_FALSE(consumer.start());
    }
}

TEST(PulsarConsumerTest, DecodeJsonReusesKafkaFormat) {
    const char* json = R"({"symbol_id":7,"price":2500,"volume":42,"ts":111222333})";
    auto result = PulsarConsumer::decode_json(json, std::strlen(json));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 7u);
    EXPECT_EQ(result->price, 2500);
    EXPECT_EQ(result->volume, 42u);
    EXPECT_EQ(result->recv_ts, 111222333);
}

TEST(PulsarConsumerTest, DecodeJsonNullOrEmpty) {
    EXPECT_FALSE(PulsarConsumer::decode_json(nullptr, 0).has_value());
    EXPECT_FALSE(PulsarConsumer::decode_json("", 0).has_value());
}

TEST(PulsarConsumerTest, DecodeBinaryExact) {
    TickMessage original{};
    original.symbol_id = 42;
    original.price = 99000;
    original.volume = 200;
    original.recv_ts = 9876543210LL;
    original.msg_type = 1;

    auto result = PulsarConsumer::decode_binary(
        reinterpret_cast<const char*>(&original), sizeof(original));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 42u);
    EXPECT_EQ(result->price, 99000);
    EXPECT_EQ(result->volume, 200u);
    EXPECT_EQ(result->recv_ts, 9876543210LL);
    EXPECT_EQ(result->msg_type, 1);
}

TEST(PulsarConsumerTest, DecodeBinaryWrongSize) {
    TickMessage msg{};
    EXPECT_FALSE(PulsarConsumer::decode_binary(
        reinterpret_cast<const char*>(&msg), sizeof(msg) - 1).has_value());
}

TEST(PulsarConsumerTest, DecodeJsonHumanKnownSymbol) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"arm_joint", 10}};
    const char* json = R"({"symbol":"arm_joint","price":23.75,"volume":1,"ts":55})";

    auto result = PulsarConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 100.0);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 10u);
    EXPECT_EQ(result->price, 2375);
    EXPECT_EQ(result->volume, 1u);
    EXPECT_EQ(result->recv_ts, 55);
}

TEST(PulsarConsumerTest, DecodeJsonHumanUnknownSymbol) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"arm_joint", 10}};
    const char* json = R"({"symbol":"missing","price":1.0,"volume":1})";
    auto result = PulsarConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 100.0);
    EXPECT_FALSE(result.has_value());
}

TEST(PulsarConsumerTest, IngestDecodedNoPipeline) {
    PulsarConfig cfg;
    cfg.topic = "t";
    PulsarConsumer consumer(cfg);

    TickMessage msg{};
    msg.symbol_id = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 0u);
}

TEST(PulsarConsumerTest, IngestDecodedSingleNode) {
    PulsarConfig cfg;
    cfg.topic = "t";
    PulsarConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price = 15000;
    msg.volume = 100;
    EXPECT_TRUE(consumer.ingest_decoded(msg));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.route_local, 1u);
    EXPECT_EQ(stats.route_remote, 0u);
    EXPECT_EQ(stats.ingest_failures, 0u);
}

TEST(PulsarConsumerTest, RoutingRemoteTargetMissingRpcClient) {
    PulsarConfig cfg;
    cfg.topic = "t";
    cfg.backpressure_retries = 0;
    cfg.backpressure_sleep_us = 0;
    PulsarConsumer consumer(cfg);

    auto router = std::make_shared<zeptodb::cluster::PartitionRouter>();
    router->add_node(7);
    consumer.set_routing(1, router, {});

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price = 100;
    EXPECT_FALSE(consumer.ingest_decoded(msg));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.route_remote, 0u);
    EXPECT_EQ(stats.ingest_failures, 1u);
}

TEST(PulsarConsumerTest, OnMessageUpdatesStats) {
    PulsarConfig cfg;
    cfg.topic = "t";
    cfg.format = MessageFormat::JSON;
    PulsarConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    const char* valid = R"({"symbol_id":1,"price":15000,"volume":100})";
    EXPECT_TRUE(consumer.on_message(valid, std::strlen(valid)));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.messages_consumed, 1u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_GT(stats.bytes_consumed, 0u);
    EXPECT_EQ(stats.route_local, 1u);
}

TEST(PulsarConsumerTest, OnMessageDecodeErrorIncrementsStats) {
    PulsarConfig cfg;
    cfg.topic = "t";
    cfg.format = MessageFormat::JSON;
    PulsarConsumer consumer(cfg);

    const char* garbage = "{{not valid json}}";
    EXPECT_FALSE(consumer.on_message(garbage, std::strlen(garbage)));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.messages_consumed, 0u);
    EXPECT_EQ(stats.decode_errors, 1u);
}

TEST(PulsarConsumerTest, TableScopedIngestLandsInTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute("CREATE TABLE pulsar_feed "
                              "(symbol INT64, price INT64, volume INT64, "
                              "timestamp TIMESTAMP_NS)");
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t tid = pipeline.schema_registry().get_table_id("pulsar_feed");
    ASSERT_NE(tid, 0);

    PulsarConfig cfg;
    cfg.topic = "t";
    cfg.table_name = "pulsar_feed";
    PulsarConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price = 12345;
    msg.volume = 10;
    EXPECT_TRUE(consumer.ingest_decoded(msg));

    pipeline.drain_sync(100);

    auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
    ASSERT_GE(parts.size(), 1u);
    size_t total = 0;
    for (auto* p : parts) total += p->num_rows();
    EXPECT_GE(total, 1u);
}

TEST(PulsarConsumerTest, UnknownTableDropsMessages) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    PulsarConfig cfg;
    cfg.topic = "t";
    cfg.table_name = "missing";
    PulsarConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 3;
    msg.price = 1;
    msg.volume = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 1u);
}

TEST(PulsarConsumerTest, FormatPrometheusCounters) {
    PulsarStats stats;
    stats.messages_consumed = 1000;
    stats.bytes_consumed = 64000;
    stats.decode_errors = 3;
    stats.route_local = 900;
    stats.route_remote = 100;
    stats.ingest_failures = 2;
    stats.receive_timeouts = 4;
    stats.receive_errors = 5;
    stats.ack_errors = 6;

    const std::string out = PulsarConsumer::format_prometheus("topic-a", stats);

    EXPECT_NE(out.find("zepto_pulsar_messages_consumed_total{consumer=\"topic-a\"} 1000"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_bytes_consumed_total{consumer=\"topic-a\"} 64000"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_decode_errors_total{consumer=\"topic-a\"} 3"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_route_local_total{consumer=\"topic-a\"} 900"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_route_remote_total{consumer=\"topic-a\"} 100"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_ingest_failures_total{consumer=\"topic-a\"} 2"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_receive_timeouts_total{consumer=\"topic-a\"} 4"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_receive_errors_total{consumer=\"topic-a\"} 5"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_pulsar_ack_errors_total{consumer=\"topic-a\"} 6"),
              std::string::npos);
}

TEST(PulsarConsumerTest, StopIsIdempotent) {
    PulsarConfig cfg;
    cfg.topic = "t";
    PulsarConsumer consumer(cfg);

    consumer.stop();
    consumer.stop();
    EXPECT_FALSE(consumer.is_running());
}

TEST(PulsarConsumerTest, StartWithoutPulsarLibrary) {
    PulsarConfig cfg;
    cfg.topic = "persistent://public/default/market-data";
    PulsarConsumer consumer(cfg);

#ifdef ZEPTO_PULSAR_AVAILABLE
    (void)consumer.start();
    consumer.stop();
#else
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
#endif
}

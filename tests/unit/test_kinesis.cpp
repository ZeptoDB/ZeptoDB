// ============================================================================
// ZeptoDB: KinesisConsumer unit tests
// ============================================================================
// Tests exercise decode / routing logic without live AWS credentials.
// ============================================================================

#include "zeptodb/feeds/kinesis_consumer.h"

#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

#include <cstring>

#include <gtest/gtest.h>

using namespace zeptodb::feeds;
using zeptodb::ingestion::TickMessage;

TEST(KinesisConsumerTest, ConfigDefaults) {
    KinesisConfig cfg;
    cfg.stream_name = "market-data";

    EXPECT_EQ(cfg.region, "us-east-1");
    EXPECT_EQ(cfg.shard_id, "shardId-000000000000");
    EXPECT_EQ(cfg.iterator_type, KinesisIteratorType::LATEST);
    EXPECT_EQ(cfg.poll_interval_ms, 200);
    EXPECT_EQ(cfg.max_records_per_poll, 1000);
    EXPECT_EQ(cfg.format, MessageFormat::JSON);
    EXPECT_DOUBLE_EQ(cfg.price_scale, 10000.0);
}

TEST(KinesisConsumerTest, MaxRecordsValidation) {
    EXPECT_TRUE(KinesisConsumer::is_valid_max_records(1));
    EXPECT_TRUE(KinesisConsumer::is_valid_max_records(10000));
    EXPECT_FALSE(KinesisConsumer::is_valid_max_records(0));
    EXPECT_FALSE(KinesisConsumer::is_valid_max_records(10001));
}

TEST(KinesisConsumerTest, DecodeJsonReusesKafkaFormat) {
    const char* json = R"({"symbol_id":1,"price":15000,"volume":100,"ts":123456789})";
    auto result = KinesisConsumer::decode_json(json, std::strlen(json));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 1u);
    EXPECT_EQ(result->price, 15000);
    EXPECT_EQ(result->volume, 100u);
    EXPECT_EQ(result->recv_ts, 123456789);
}

TEST(KinesisConsumerTest, DecodeJsonNullOrEmpty) {
    EXPECT_FALSE(KinesisConsumer::decode_json(nullptr, 0).has_value());
    EXPECT_FALSE(KinesisConsumer::decode_json("", 0).has_value());
}

TEST(KinesisConsumerTest, DecodeBinaryExact) {
    TickMessage original{};
    original.symbol_id = 42;
    original.price = 99000;
    original.volume = 200;
    original.recv_ts = 9876543210LL;
    original.msg_type = 1;

    auto result = KinesisConsumer::decode_binary(
        reinterpret_cast<const char*>(&original), sizeof(original));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 42u);
    EXPECT_EQ(result->price, 99000);
    EXPECT_EQ(result->volume, 200u);
    EXPECT_EQ(result->recv_ts, 9876543210LL);
    EXPECT_EQ(result->msg_type, 1);
}

TEST(KinesisConsumerTest, DecodeJsonHumanKnownSymbol) {
    std::unordered_map<std::string, zeptodb::SymbolId> sym_map{{"AAPL", 1}};
    const char* json = R"({"symbol":"AAPL","price":150.50,"volume":300,"ts":111})";

    auto result = KinesisConsumer::decode_json_human(
        json, std::strlen(json), sym_map, 10000.0);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->symbol_id, 1u);
    EXPECT_EQ(result->price, 1505000);
    EXPECT_EQ(result->volume, 300u);
    EXPECT_EQ(result->recv_ts, 111);
}

TEST(KinesisConsumerTest, IngestDecodedNoPipeline) {
    KinesisConfig cfg;
    cfg.stream_name = "s";
    KinesisConsumer consumer(cfg);

    TickMessage msg{};
    msg.symbol_id = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
}

TEST(KinesisConsumerTest, IngestDecodedSingleNode) {
    KinesisConfig cfg;
    cfg.stream_name = "s";
    KinesisConsumer consumer(cfg);

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

TEST(KinesisConsumerTest, OnRecordUpdatesStats) {
    KinesisConfig cfg;
    cfg.stream_name = "s";
    cfg.format = MessageFormat::JSON;
    KinesisConsumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    const char* valid = R"({"symbol_id":1,"price":15000,"volume":100})";
    EXPECT_TRUE(consumer.on_record(valid, std::strlen(valid)));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.records_consumed, 1u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_GT(stats.bytes_consumed, 0u);
    EXPECT_EQ(stats.route_local, 1u);
}

TEST(KinesisConsumerTest, OnRecordDecodeErrorIncrementsStats) {
    KinesisConfig cfg;
    cfg.stream_name = "s";
    cfg.format = MessageFormat::JSON;
    KinesisConsumer consumer(cfg);

    const char* garbage = "{{not valid json}}";
    EXPECT_FALSE(consumer.on_record(garbage, std::strlen(garbage)));

    auto stats = consumer.stats();
    EXPECT_EQ(stats.records_consumed, 0u);
    EXPECT_EQ(stats.decode_errors, 1u);
}

TEST(KinesisConsumerTest, TableScopedIngestLandsInTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute("CREATE TABLE kinesis_feed "
                              "(symbol INT64, price INT64, volume INT64, "
                              "timestamp TIMESTAMP_NS)");
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t tid = pipeline.schema_registry().get_table_id("kinesis_feed");
    ASSERT_NE(tid, 0);

    KinesisConfig cfg;
    cfg.stream_name = "s";
    cfg.table_name = "kinesis_feed";
    KinesisConsumer consumer(cfg);
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

TEST(KinesisConsumerTest, UnknownTableDropsMessages) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    KinesisConfig cfg;
    cfg.stream_name = "s";
    cfg.table_name = "missing";
    KinesisConsumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 3;
    msg.price = 1;
    msg.volume = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 1u);
}

TEST(KinesisConsumerTest, FormatPrometheusCounters) {
    KinesisStats stats;
    stats.records_consumed = 1000;
    stats.bytes_consumed = 64000;
    stats.decode_errors = 3;
    stats.route_local = 900;
    stats.route_remote = 100;
    stats.ingest_failures = 2;
    stats.get_records_errors = 4;

    const std::string out = KinesisConsumer::format_prometheus("stream-a", stats);

    EXPECT_NE(out.find("zepto_kinesis_records_consumed_total{consumer=\"stream-a\"} 1000"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_bytes_consumed_total{consumer=\"stream-a\"} 64000"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_decode_errors_total{consumer=\"stream-a\"} 3"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_route_local_total{consumer=\"stream-a\"} 900"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_route_remote_total{consumer=\"stream-a\"} 100"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_ingest_failures_total{consumer=\"stream-a\"} 2"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_kinesis_get_records_errors_total{consumer=\"stream-a\"} 4"),
              std::string::npos);
}

TEST(KinesisConsumerTest, StartWithoutKinesisLibrary) {
    KinesisConfig cfg;
    cfg.stream_name = "s";
    KinesisConsumer consumer(cfg);

#ifdef ZEPTO_KINESIS_AVAILABLE
    (void)consumer.start();
    consumer.stop();
#else
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
#endif
}

// ============================================================================
// ZeptoDB: Ros2Consumer unit tests
// ============================================================================
// Tests exercise config validation, pure time/sample mapping, stats, and
// ZeptoDB routing. When compiled with ROS 2, this file also runs a small
// live rclcpp pub/sub smoke into an in-memory ZeptoDB table.
// ============================================================================

#include "zeptodb/feeds/ros2_consumer.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifdef ZEPTO_ROS2_AVAILABLE
#include "zeptodb/auth/license_validator.h"

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float64.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#endif

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
#include <rclcpp/serialization.hpp>
#include <rosbag2_cpp/writer.hpp>

#include <filesystem>
#endif

using namespace zeptodb::feeds;
using zeptodb::ingestion::TickMessage;

namespace {

Ros2Config valid_config() {
    Ros2Config cfg;
    Ros2SubscriptionConfig sub;
    sub.topic = "/robot/joint_effort";
    sub.message_type = "std_msgs/msg/Float64";
    sub.fields.push_back(Ros2FieldMapping{"data", 1, 1000.0});
    cfg.subscriptions.push_back(sub);
    return cfg;
}

#ifdef ZEPTO_ROS2_AVAILABLE
class TrialLicenseGuard {
public:
    TrialLicenseGuard() {
        const std::string key =
            zeptodb::auth::LicenseValidator::generate_trial_key("ROS2Test");
        zeptodb::auth::license().load_from_jwt_string_for_testing(key);
    }

    ~TrialLicenseGuard() {
        zeptodb::auth::license().load_from_jwt_string_for_testing("");
    }
};
#endif

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
std::filesystem::path make_temp_bag_path(const std::string& name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                (name + "_" + std::to_string(suffix));
    std::filesystem::remove_all(path);
    return path;
}

void write_float64_bag(
    const std::filesystem::path& uri,
    const std::vector<std::pair<std::string, double>>& samples) {
    rosbag2_cpp::Writer writer;
    rosbag2_storage::StorageOptions options;
    options.uri = uri.string();
    options.storage_id = "sqlite3";
    writer.open(options);

    rclcpp::Serialization<std_msgs::msg::Float64> serializer;
    rcutils_time_point_value_t timestamp = 1'000'000;
    for (const auto& [topic, value] : samples) {
        auto serialized = std::make_shared<rclcpp::SerializedMessage>();
        std_msgs::msg::Float64 msg;
        msg.data = value;
        serializer.serialize_message(&msg, serialized.get());
        writer.write(serialized, topic, "std_msgs/msg/Float64", timestamp, timestamp);
        timestamp += 1'000'000;
    }

    writer.close();
}
#endif

} // namespace

TEST(Ros2ConsumerTest, ConfigDefaults) {
    Ros2Config cfg = valid_config();

    EXPECT_EQ(cfg.node_name, "zepto_ros2_bridge");
    EXPECT_EQ(cfg.clock_domain, Ros2ClockDomain::Unknown);
    EXPECT_EQ(cfg.backpressure_retries, 3);
    EXPECT_EQ(cfg.backpressure_sleep_us, 100);
    ASSERT_EQ(cfg.subscriptions.size(), 1u);
    EXPECT_EQ(cfg.subscriptions[0].mode, Ros2IngestMode::ScalarFields);
    EXPECT_EQ(cfg.subscriptions[0].queue_capacity, 1024u);
    EXPECT_TRUE(cfg.subscriptions[0].drop_on_full);
}

TEST(Ros2ConsumerTest, ValidateAcceptsMinimalScalarConfig) {
    EXPECT_FALSE(Ros2Consumer::validate_config(valid_config()).has_value());
}

TEST(Ros2ConsumerTest, ValidateAcceptsMinimalBagConfig) {
    Ros2BagConfig bag;
    bag.uri = "/tmp/robot_run";
    EXPECT_FALSE(Ros2Consumer::validate_bag_config(bag).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsInvalidBagConfig) {
    Ros2BagConfig bag;
    EXPECT_TRUE(Ros2Consumer::validate_bag_config(bag).has_value());

    bag.uri = "/tmp/robot_run";
    bag.replay_speed = 0.0;
    EXPECT_TRUE(Ros2Consumer::validate_bag_config(bag).has_value());

    bag.replay_speed = 1.0;
    bag.topics = {"/a", "/a"};
    EXPECT_TRUE(Ros2Consumer::validate_bag_config(bag).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsEmptySubscriptions) {
    Ros2Config cfg;
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsEmptyTopic) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].topic.clear();
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsDuplicateTopic) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions.push_back(cfg.subscriptions[0]);
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsZeroSymbolId) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].fields[0].symbol_id = 0;
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsBadScale) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].fields[0].value_scale = std::nan("");
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());

    cfg = valid_config();
    cfg.subscriptions[0].fields[0].value_scale = 0.0;
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, SupportedLiveScalarTypes) {
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/Float64"));
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/Float32"));
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/Int64"));
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/Int32"));
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/UInt64"));
    EXPECT_TRUE(Ros2Consumer::is_supported_live_scalar_type("std_msgs/msg/UInt32"));
    EXPECT_FALSE(Ros2Consumer::is_supported_live_scalar_type("sensor_msgs/msg/JointState"));
}

TEST(Ros2ConsumerTest, ValidateRejectsUnsupportedLiveScalarType) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].message_type = "sensor_msgs/msg/JointState";
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsNonDataFieldForLiveScalarType) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].fields[0].field_path = "effort";
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ScaleNumericValueHandlesScaleAndTruncation) {
    auto scaled = Ros2Consumer::scale_numeric_value(42.75L, 100.0L);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(*scaled, 4275);
}

TEST(Ros2ConsumerTest, ScaleNumericValueRejectsInvalidValues) {
    EXPECT_FALSE(Ros2Consumer::scale_numeric_value(
        std::numeric_limits<long double>::quiet_NaN(), 1.0L).has_value());
    EXPECT_FALSE(Ros2Consumer::scale_numeric_value(1.0L, 0.0L).has_value());
    EXPECT_FALSE(Ros2Consumer::scale_numeric_value(1.0e30L, 1.0L).has_value());
}

TEST(Ros2ConsumerTest, TimeToNsHandlesEpochAndNormalTime) {
    auto zero = Ros2Consumer::time_to_ns(Ros2Time{0, 0});
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(*zero, 0u);

    auto t = Ros2Consumer::time_to_ns(Ros2Time{12, 345});
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, 12'000'000'345ull);
}

TEST(Ros2ConsumerTest, TimeToNsRejectsInvalidInput) {
    EXPECT_FALSE(Ros2Consumer::time_to_ns(Ros2Time{-1, 0}).has_value());
    EXPECT_FALSE(Ros2Consumer::time_to_ns(Ros2Time{1, 1'000'000'000u}).has_value());
}

TEST(Ros2ConsumerTest, ScalarSampleToTickMapsFields) {
    Ros2ScalarSample sample;
    sample.topic = "/robot/joint_effort";
    sample.symbol_id = 7;
    sample.value = 42000;
    sample.source_ts_ns = 123;
    sample.recv_ts_ns = 456;
    sample.quality = 9;

    auto tick = Ros2Consumer::scalar_sample_to_tick(sample);
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->symbol_id, 7u);
    EXPECT_EQ(tick->price, 42000);
    EXPECT_EQ(tick->recv_ts, 123);
    EXPECT_EQ(tick->volume, 9u);
}

TEST(Ros2ConsumerTest, ScalarSampleToTickFallsBackToReceiveTime) {
    Ros2ScalarSample sample;
    sample.symbol_id = 7;
    sample.source_ts_ns = 0;
    sample.recv_ts_ns = 456;

    auto tick = Ros2Consumer::scalar_sample_to_tick(sample);
    ASSERT_TRUE(tick.has_value());
    EXPECT_EQ(tick->recv_ts, 456);
}

TEST(Ros2ConsumerTest, ScalarSampleToTickRejectsZeroSymbolId) {
    Ros2ScalarSample sample;
    sample.symbol_id = 0;
    EXPECT_FALSE(Ros2Consumer::scalar_sample_to_tick(sample).has_value());
}

TEST(Ros2ConsumerTest, IngestDecodedNoPipeline) {
    Ros2Consumer consumer(valid_config());

    TickMessage msg{};
    msg.symbol_id = 1;
    EXPECT_FALSE(consumer.ingest_decoded(msg));
    EXPECT_EQ(consumer.stats().ingest_failures, 1u);
}

TEST(Ros2ConsumerTest, IngestDecodedSingleNode) {
    Ros2Config cfg = valid_config();
    cfg.backpressure_retries = 0;
    cfg.backpressure_sleep_us = 0;
    Ros2Consumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price = 100;
    msg.volume = 1;

    EXPECT_TRUE(consumer.ingest_decoded(msg));
    auto s = consumer.stats();
    EXPECT_EQ(s.route_local, 1u);
    EXPECT_EQ(s.rows_ingested, 1u);
    EXPECT_EQ(s.ingest_failures, 0u);
}

TEST(Ros2ConsumerTest, OnScalarSampleUnknownTopicCountsDecodeError) {
    Ros2Consumer consumer(valid_config());

    Ros2ScalarSample sample;
    sample.topic = "/unknown";
    sample.symbol_id = 1;
    sample.value = 10;

    EXPECT_FALSE(consumer.on_scalar_sample(sample));
    EXPECT_EQ(consumer.stats().decode_errors, 1u);
}

TEST(Ros2ConsumerTest, OnScalarSampleUpdatesStatsAndLag) {
    Ros2Config cfg = valid_config();
    cfg.backpressure_retries = 0;
    cfg.backpressure_sleep_us = 0;
    Ros2Consumer consumer(cfg);

    zeptodb::core::ZeptoPipeline pipeline;
    consumer.set_pipeline(&pipeline);

    Ros2ScalarSample sample;
    sample.topic = "/robot/joint_effort";
    sample.symbol_id = 1;
    sample.value = 10;
    sample.source_ts_ns = 100;
    sample.recv_ts_ns = 175;

    EXPECT_TRUE(consumer.on_scalar_sample(sample));
    auto s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 1u);
    EXPECT_EQ(s.rows_ingested, 1u);
    EXPECT_EQ(s.bytes_consumed, sizeof(int64_t));
    EXPECT_EQ(s.last_source_lag_ns, 75);
}

TEST(Ros2ConsumerTest, TableScopedScalarIngestLandsInTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE ros_joint (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_joint");
    ASSERT_NE(tid, 0);

    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].table_name = "ros_joint";
    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    Ros2ScalarSample sample;
    sample.topic = "/robot/joint_effort";
    sample.symbol_id = 1;
    sample.value = 123;

    EXPECT_TRUE(consumer.on_scalar_sample(sample));

    pipeline.drain_sync(100);
    auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
    ASSERT_GE(parts.size(), 1u);
}

TEST(Ros2ConsumerTest, UnknownTableDropsScalarSample) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].table_name = "missing_table";
    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    Ros2ScalarSample sample;
    sample.topic = "/robot/joint_effort";
    sample.symbol_id = 1;
    sample.value = 123;

    EXPECT_FALSE(consumer.on_scalar_sample(sample));
    auto s = consumer.stats();
    EXPECT_EQ(s.messages_consumed, 1u);
    EXPECT_EQ(s.messages_dropped, 1u);
    EXPECT_EQ(s.ingest_failures, 1u);
}

TEST(Ros2ConsumerTest, FormatPrometheusCounters) {
    Ros2Stats s;
    s.messages_consumed = 10;
    s.rows_ingested = 9;
    s.bytes_consumed = 80;
    s.decode_errors = 1;
    s.messages_dropped = 2;
    s.route_local = 8;
    s.route_remote = 1;
    s.ingest_failures = 2;
    s.last_source_lag_ns = 75;

    const std::string out = Ros2Consumer::format_prometheus("robot-lab", s);
    EXPECT_NE(out.find("zepto_ros2_messages_consumed_total{bridge=\"robot-lab\"} 10"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_ros2_rows_ingested_total{bridge=\"robot-lab\"} 9"),
              std::string::npos);
    EXPECT_NE(out.find("zepto_ros2_source_lag_ns{bridge=\"robot-lab\"} 75"),
              std::string::npos);
}

TEST(Ros2ConsumerTest, StartFailsClosedWithoutIotLicense) {
    Ros2Consumer consumer(valid_config());
    EXPECT_FALSE(consumer.start());
    EXPECT_FALSE(consumer.is_running());
}

TEST(Ros2ConsumerTest, StopIsIdempotent) {
    Ros2Consumer consumer(valid_config());
    consumer.stop();
    consumer.stop();
    EXPECT_FALSE(consumer.is_running());
}

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
TEST(Ros2ConsumerTest, ImportBagIngestsConfiguredScalarTopic) {
    TrialLicenseGuard license;

    const auto bag_path = make_temp_bag_path("zepto_ros2_bag_import");
    write_float64_bag(
        bag_path,
        {{"/zepto_ros2/bag_scalar", 42.5},
         {"/zepto_ros2/ignored_scalar", 9.0}});

    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE ros_bag (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_bag");
    ASSERT_NE(tid, 0);

    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].topic = "/zepto_ros2/bag_scalar";
    cfg.subscriptions[0].table_name = "ros_bag";
    cfg.subscriptions[0].fields[0].symbol_id = 77;
    cfg.subscriptions[0].fields[0].value_scale = 10.0;

    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    Ros2BagConfig bag;
    bag.uri = bag_path.string();
    const auto stats = consumer.import_bag(bag);

    pipeline.drain_sync(100);
    const auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
    std::filesystem::remove_all(bag_path);

    EXPECT_TRUE(stats.completed) << stats.error;
    EXPECT_EQ(stats.messages_read, 1u);
    EXPECT_EQ(stats.messages_consumed, 1u);
    EXPECT_EQ(stats.rows_ingested, 1u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_EQ(stats.first_source_ts_ns, 1'000'000u);
    EXPECT_EQ(stats.last_source_ts_ns, 1'000'000u);
    EXPECT_GE(parts.size(), 1u);
}

TEST(Ros2ConsumerTest, ImportBagCanSkipUnknownExplicitTopic) {
    TrialLicenseGuard license;

    const auto bag_path = make_temp_bag_path("zepto_ros2_bag_skip");
    write_float64_bag(
        bag_path,
        {{"/zepto_ros2/bag_scalar", 1.0},
         {"/zepto_ros2/other_scalar", 2.0}});

    zeptodb::core::ZeptoPipeline pipeline;
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].topic = "/zepto_ros2/bag_scalar";

    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    Ros2BagConfig bag;
    bag.uri = bag_path.string();
    bag.topics = {"/zepto_ros2/bag_scalar", "/zepto_ros2/other_scalar"};
    const auto stats = consumer.import_bag(bag);

    std::filesystem::remove_all(bag_path);

    EXPECT_TRUE(stats.completed) << stats.error;
    EXPECT_EQ(stats.messages_read, 2u);
    EXPECT_EQ(stats.messages_consumed, 1u);
    EXPECT_EQ(stats.messages_skipped, 1u);
    EXPECT_EQ(stats.decode_errors, 0u);
}
#endif

#ifdef ZEPTO_ROS2_AVAILABLE
TEST(Ros2ConsumerTest, LiveFloat64SubscriberIngestsIntoTable) {
#if defined(__unix__) || defined(__APPLE__)
    setenv("ROS_DOMAIN_ID", "78", 1);
#endif
    TrialLicenseGuard license;

    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE ros_live (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_live");
    ASSERT_NE(tid, 0);

    Ros2Config cfg = valid_config();
    cfg.node_name = "zepto_ros2_live_test";
    cfg.subscriptions[0].topic = "/zepto_ros2/live_scalar";
    cfg.subscriptions[0].table_name = "ros_live";
    cfg.subscriptions[0].queue_capacity = 16;
    cfg.subscriptions[0].drop_on_full = false;
    cfg.subscriptions[0].fields[0].symbol_id = 42;
    cfg.subscriptions[0].fields[0].value_scale = 100.0;

    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);
    const bool started = consumer.start();
    EXPECT_TRUE(started);
    if (!started) {
        return;
    }

    auto context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions node_options;
    node_options.context(context);
    auto node = std::make_shared<rclcpp::Node>("zepto_ros2_live_test_pub", node_options);
    auto pub = node->create_publisher<std_msgs::msg::Float64>(
        "/zepto_ros2/live_scalar", rclcpp::QoS(rclcpp::KeepLast(10)).reliable());

    using namespace std::chrono_literals;
    for (int i = 0; i < 40 && pub->get_subscription_count() == 0; ++i) {
        rclcpp::spin_some(node);
        std::this_thread::sleep_for(50ms);
    }

    bool observed = false;
    for (int i = 0; i < 100; ++i) {
        std_msgs::msg::Float64 msg;
        msg.data = 42.5;
        pub->publish(msg);
        rclcpp::spin_some(node);
        pipeline.drain_sync(100);

        const auto stats = consumer.stats();
        const auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
        if (stats.messages_consumed >= 1 && stats.rows_ingested >= 1 && !parts.empty()) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    pipeline.drain_sync(100);
    const auto stats = consumer.stats();
    const auto parts = pipeline.partition_manager().get_partitions_for_table(tid);

    consumer.stop();
    if (context->is_valid()) {
        context->shutdown("ROS 2 live test complete");
    }

    EXPECT_TRUE(observed);
    EXPECT_GE(stats.messages_consumed, 1u);
    EXPECT_GE(stats.rows_ingested, 1u);
    EXPECT_GE(parts.size(), 1u);
}
#endif

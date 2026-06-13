// ============================================================================
// ZeptoDB: Ros2Consumer unit tests
// ============================================================================
// Tests exercise config validation, pure time/sample mapping, stats, and
// ZeptoDB routing. When compiled with ROS 2, this file also runs a small
// live rclcpp pub/sub smoke into an in-memory ZeptoDB table.
// ============================================================================

#include "zeptodb/feeds/ros2_consumer.h"
#include "zeptodb/cluster/rpc_protocol.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/partition_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
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

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
#include <sensor_msgs/msg/imu.hpp>
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

Ros2Config standard_profile_config(
    const std::string& message_type,
    std::vector<Ros2FieldMapping> fields) {
    Ros2Config cfg;
    Ros2SubscriptionConfig sub;
    sub.topic = "/robot/profile";
    sub.message_type = message_type;
    sub.mode = Ros2IngestMode::StandardProfile;
    sub.fields = std::move(fields);
    cfg.subscriptions.push_back(std::move(sub));
    return cfg;
}

Ros2Config typed_profile_config(
    const std::string& message_type,
    const std::string& table_name = "ros_typed") {
    Ros2Config cfg;
    Ros2SubscriptionConfig sub;
    sub.topic = "/robot/profile";
    sub.message_type = message_type;
    sub.mode = Ros2IngestMode::TypedProfile;
    sub.table_name = table_name;
    sub.typed_partition_symbol_id = 900;
    cfg.subscriptions.push_back(std::move(sub));
    return cfg;
}

const char* sql_type_name(zeptodb::storage::ColumnType type) {
    using zeptodb::storage::ColumnType;
    switch (type) {
        case ColumnType::INT32:        return "INT32";
        case ColumnType::INT64:        return "INT64";
        case ColumnType::FLOAT32:      return "FLOAT32";
        case ColumnType::FLOAT64:      return "FLOAT64";
        case ColumnType::TIMESTAMP_NS: return "TIMESTAMP_NS";
        case ColumnType::SYMBOL:       return "SYMBOL";
        case ColumnType::BOOL:         return "BOOL";
        case ColumnType::STRING:       return "STRING";
    }
    return "INT64";
}

std::string typed_profile_create_sql(
    const std::string& table_name,
    Ros2StandardProfile profile,
    const std::vector<zeptodb::storage::ColumnDef>& extra_columns = {}) {
    std::ostringstream sql;
    sql << "CREATE TABLE " << table_name << " (";
    auto columns = Ros2Consumer::typed_profile_schema(profile);
    columns.insert(columns.end(), extra_columns.begin(), extra_columns.end());
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i != 0) sql << ", ";
        sql << columns[i].name << " " << sql_type_name(columns[i].type);
    }
    sql << ")";
    return sql.str();
}

class CountingRos2RpcClient : public zeptodb::cluster::RpcClientBase {
public:
    std::atomic<int> tick_calls{0};
    std::atomic<int> typed_row_calls{0};
    bool next_result = true;
    zeptodb::core::TypedRowMessage last_row;

    bool ingest_tick(const zeptodb::ingestion::TickMessage&) override {
        tick_calls.fetch_add(1, std::memory_order_relaxed);
        return next_result;
    }

    bool ingest_typed_row(const zeptodb::core::TypedRowMessage& row) override {
        typed_row_calls.fetch_add(1, std::memory_order_relaxed);
        last_row = row;
        return next_result;
    }
};

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

TEST(Ros2ConsumerTest, SupportedStandardProfileTypes) {
    EXPECT_EQ(Ros2Consumer::standard_profile_for_message_type("sensor_msgs/msg/Imu"),
              Ros2StandardProfile::Imu);
    EXPECT_EQ(Ros2Consumer::standard_profile_for_message_type("sensor_msgs/msg/JointState"),
              Ros2StandardProfile::JointState);
    EXPECT_EQ(Ros2Consumer::standard_profile_for_message_type("nav_msgs/msg/Odometry"),
              Ros2StandardProfile::Odometry);
    EXPECT_EQ(Ros2Consumer::standard_profile_for_message_type("tf2_msgs/msg/TFMessage"),
              Ros2StandardProfile::TfMessage);
    EXPECT_EQ(Ros2Consumer::standard_profile_for_message_type("sensor_msgs/msg/LaserScan"),
              Ros2StandardProfile::LaserScan);
    EXPECT_TRUE(Ros2Consumer::is_supported_standard_profile_type("sensor_msgs/msg/Imu"));
    EXPECT_FALSE(Ros2Consumer::is_supported_standard_profile_type("std_msgs/msg/Float64"));
}

TEST(Ros2ConsumerTest, ValidateRejectsUnsupportedLiveScalarType) {
    Ros2Config cfg = valid_config();
    cfg.subscriptions[0].message_type = "sensor_msgs/msg/JointState";
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateAcceptsStandardProfileConfigs) {
    EXPECT_FALSE(Ros2Consumer::validate_config(
        standard_profile_config("sensor_msgs/msg/Imu",
            {{"orientation.w", 10, 1000.0},
             {"angular_velocity.z", 11, 1000.0},
             {"linear_acceleration.x", 12, 1000.0}})).has_value());

    EXPECT_FALSE(Ros2Consumer::validate_config(
        standard_profile_config("sensor_msgs/msg/JointState",
            {{"position", 20, 1000.0}, {"velocity", 30, 1000.0}})).has_value());

    EXPECT_FALSE(Ros2Consumer::validate_config(
        standard_profile_config("nav_msgs/msg/Odometry",
            {{"pose.position.x", 40, 1000.0},
             {"twist.linear.x", 41, 1000.0}})).has_value());

    EXPECT_FALSE(Ros2Consumer::validate_config(
        standard_profile_config("tf2_msgs/msg/TFMessage",
            {{"translation.x", 50, 1000.0}, {"rotation.w", 51, 1000.0}})).has_value());

    EXPECT_FALSE(Ros2Consumer::validate_config(
        standard_profile_config("sensor_msgs/msg/LaserScan",
            {{"ranges.count", 60, 1.0}, {"ranges.mean", 61, 1000.0}})).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsBadStandardProfileField) {
    auto cfg = standard_profile_config(
        "sensor_msgs/msg/Imu",
        {{"pose.position.x", 10, 1.0}});
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());

    cfg = standard_profile_config(
        "std_msgs/msg/Float64",
        {{"data", 10, 1.0}});
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateAcceptsTypedProfileConfig) {
    auto cfg = typed_profile_config("sensor_msgs/msg/Imu", "ros_imu_typed");
    cfg.robot_id = "arm-01";
    cfg.session_id = "bench-a";

    EXPECT_FALSE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, ValidateRejectsBadTypedProfileConfig) {
    auto cfg = typed_profile_config("std_msgs/msg/Float64", "ros_typed");
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());

    cfg = typed_profile_config("sensor_msgs/msg/Imu", "");
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());

    cfg = typed_profile_config("sensor_msgs/msg/Imu", "ros_typed");
    cfg.subscriptions[0].fields.push_back({"orientation.w", 10, 1.0});
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());

    cfg = typed_profile_config("sensor_msgs/msg/Imu", "ros_typed");
    cfg.subscriptions[0].typed_partition_symbol_id = 0;
    EXPECT_TRUE(Ros2Consumer::validate_config(cfg).has_value());
}

TEST(Ros2ConsumerTest, TypedProfileSchemaListsImuColumns) {
    const auto columns = Ros2Consumer::typed_profile_schema(Ros2StandardProfile::Imu);
    auto has_column = [&](const std::string& name, zeptodb::storage::ColumnType type) {
        return std::any_of(columns.begin(), columns.end(), [&](const auto& column) {
            return column.name == name && column.type == type;
        });
    };

    EXPECT_TRUE(has_column("timestamp", zeptodb::storage::ColumnType::TIMESTAMP_NS));
    EXPECT_TRUE(has_column("robot_id", zeptodb::storage::ColumnType::SYMBOL));
    EXPECT_TRUE(has_column("frame_id", zeptodb::storage::ColumnType::SYMBOL));
    EXPECT_TRUE(has_column("orientation_w", zeptodb::storage::ColumnType::FLOAT64));
    EXPECT_TRUE(has_column("linear_acceleration_x", zeptodb::storage::ColumnType::FLOAT64));
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

TEST(Ros2ConsumerTest, ImuProfileMapsConfiguredFields) {
    const auto cfg = standard_profile_config(
        "sensor_msgs/msg/Imu",
        {{"orientation.w", 10, 1000.0},
         {"angular_velocity.z", 11, 100.0},
         {"linear_acceleration.x", 12, 10.0}});
    const auto& sub = cfg.subscriptions[0];

    Ros2ImuSample sample;
    sample.topic = "/robot/profile";
    sample.header.stamp_ns = 123;
    sample.recv_ts_ns = 456;
    sample.orientation.w = 0.707L;
    sample.angular_velocity.z = -1.25L;
    sample.linear_acceleration.x = 9.81L;

    const auto rows = Ros2Consumer::imu_sample_to_scalars(sample, sub);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].symbol_id, 10u);
    EXPECT_EQ(rows[0].value, 707);
    EXPECT_EQ(rows[0].source_ts_ns, 123u);
    EXPECT_EQ(rows[1].symbol_id, 11u);
    EXPECT_EQ(rows[1].value, -125);
    EXPECT_EQ(rows[2].symbol_id, 12u);
    EXPECT_EQ(rows[2].value, 98);
}

TEST(Ros2ConsumerTest, JointStateProfileExpandsByJointIndexAndSkipsMissingVectors) {
    const auto cfg = standard_profile_config(
        "sensor_msgs/msg/JointState",
        {{"position", 100, 1000.0}, {"effort", 200, 10.0}});
    const auto& sub = cfg.subscriptions[0];

    Ros2JointStateSample sample;
    sample.topic = "/robot/profile";
    sample.header.stamp_ns = 1;
    sample.recv_ts_ns = 2;
    sample.names = {"shoulder", "elbow", "wrist"};
    sample.positions = {1.0L, 2.0L, 3.0L};
    sample.efforts = {4.0L};

    const auto rows = Ros2Consumer::joint_state_sample_to_scalars(sample, sub);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].symbol_id, 100u);
    EXPECT_EQ(rows[0].value, 1000);
    EXPECT_EQ(rows[1].symbol_id, 200u);
    EXPECT_EQ(rows[1].value, 40);
    EXPECT_EQ(rows[2].symbol_id, 101u);
    EXPECT_EQ(rows[2].value, 2000);
    EXPECT_EQ(rows[3].symbol_id, 102u);
    EXPECT_EQ(rows[3].value, 3000);
}

TEST(Ros2ConsumerTest, OdometryProfileMapsPoseAndTwistFields) {
    const auto cfg = standard_profile_config(
        "nav_msgs/msg/Odometry",
        {{"pose.position.x", 10, 100.0},
         {"pose.orientation.w", 11, 1000.0},
         {"twist.linear.x", 12, 100.0},
         {"twist.angular.z", 13, 100.0}});
    const auto& sub = cfg.subscriptions[0];

    Ros2OdometrySample sample;
    sample.topic = "/robot/profile";
    sample.header.stamp_ns = 10;
    sample.recv_ts_ns = 20;
    sample.pose.position.x = 12.34L;
    sample.pose.orientation.w = 0.5L;
    sample.twist.linear.x = 1.25L;
    sample.twist.angular.z = -0.75L;

    const auto rows = Ros2Consumer::odometry_sample_to_scalars(sample, sub);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].value, 1234);
    EXPECT_EQ(rows[1].value, 500);
    EXPECT_EQ(rows[2].value, 125);
    EXPECT_EQ(rows[3].value, -75);
}

TEST(Ros2ConsumerTest, TfProfileExpandsTransformsByIndex) {
    const auto cfg = standard_profile_config(
        "tf2_msgs/msg/TFMessage",
        {{"translation.x", 300, 1000.0}, {"rotation.w", 400, 1000.0}});
    const auto& sub = cfg.subscriptions[0];

    Ros2TfMessageSample sample;
    sample.topic = "/robot/profile";
    sample.recv_ts_ns = 9;
    Ros2TransformSample a;
    a.header.stamp_ns = 1;
    a.translation.x = 1.0L;
    a.rotation.w = 0.1L;
    Ros2TransformSample b;
    b.header.stamp_ns = 2;
    b.translation.x = 2.0L;
    b.rotation.w = 0.2L;
    sample.transforms = {a, b};

    const auto rows = Ros2Consumer::tf_message_sample_to_scalars(sample, sub);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].symbol_id, 300u);
    EXPECT_EQ(rows[0].value, 1000);
    EXPECT_EQ(rows[1].symbol_id, 400u);
    EXPECT_EQ(rows[1].value, 100);
    EXPECT_EQ(rows[2].symbol_id, 301u);
    EXPECT_EQ(rows[2].source_ts_ns, 2u);
    EXPECT_EQ(rows[3].symbol_id, 401u);
    EXPECT_EQ(rows[3].value, 200);
}

TEST(Ros2ConsumerTest, LaserScanProfileSummarizesFiniteRanges) {
    const auto cfg = standard_profile_config(
        "sensor_msgs/msg/LaserScan",
        {{"ranges.count", 10, 1.0},
         {"ranges.min", 11, 1000.0},
         {"ranges.max", 12, 1000.0},
         {"ranges.mean", 13, 1000.0},
         {"intensities.count", 14, 1.0},
         {"range_max", 15, 1000.0}});
    const auto& sub = cfg.subscriptions[0];

    Ros2LaserScanSample sample;
    sample.topic = "/robot/profile";
    sample.header.stamp_ns = 11;
    sample.recv_ts_ns = 22;
    sample.range_max = 30.0L;
    sample.ranges = {
        1.0L,
        std::numeric_limits<long double>::infinity(),
        3.0L,
        std::numeric_limits<long double>::quiet_NaN(),
    };
    sample.intensities = {};

    const auto rows = Ros2Consumer::laser_scan_sample_to_scalars(sample, sub);
    ASSERT_EQ(rows.size(), 6u);
    EXPECT_EQ(rows[0].symbol_id, 10u);
    EXPECT_EQ(rows[0].value, 4);
    EXPECT_EQ(rows[1].symbol_id, 11u);
    EXPECT_EQ(rows[1].value, 1000);
    EXPECT_EQ(rows[2].symbol_id, 12u);
    EXPECT_EQ(rows[2].value, 3000);
    EXPECT_EQ(rows[3].symbol_id, 13u);
    EXPECT_EQ(rows[3].value, 2000);
    EXPECT_EQ(rows[4].symbol_id, 14u);
    EXPECT_EQ(rows[4].value, 0);
    EXPECT_EQ(rows[5].symbol_id, 15u);
    EXPECT_EQ(rows[5].value, 30000);
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

TEST(Ros2ConsumerTest, TypedRowIngestWritesSchemaAwareImuTable) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute(typed_profile_create_sql(
        "ros_imu_typed",
        Ros2StandardProfile::Imu,
        {{"debug_code", zeptodb::storage::ColumnType::INT32}}));
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_imu_typed");
    ASSERT_NE(tid, 0);

    const auto robot_code = pipeline.symbol_dict().intern("arm-01");
    const auto session_code = pipeline.symbol_dict().intern("bench-a");
    const auto topic_code = pipeline.symbol_dict().intern("/imu/data");
    const auto frame_code = pipeline.symbol_dict().intern("base_link");

    zeptodb::core::TypedRowMessage row;
    row.table_id = tid;
    row.symbol_id = 700;
    row.timestamp = 123'000;
    row.columns = {
        zeptodb::core::TypedColumnValue::timestamp("timestamp", 123'000),
        zeptodb::core::TypedColumnValue::timestamp("recv_ts", 123'075),
        zeptodb::core::TypedColumnValue::symbol("robot_id", robot_code),
        zeptodb::core::TypedColumnValue::symbol("session_id", session_code),
        zeptodb::core::TypedColumnValue::symbol("topic", topic_code),
        zeptodb::core::TypedColumnValue::symbol("frame_id", frame_code),
        zeptodb::core::TypedColumnValue::int32("quality", 1),
        zeptodb::core::TypedColumnValue::float64("orientation_x", 0.0),
        zeptodb::core::TypedColumnValue::float64("orientation_y", 0.0),
        zeptodb::core::TypedColumnValue::float64("orientation_z", 0.707),
        zeptodb::core::TypedColumnValue::float64("orientation_w", 0.707),
        zeptodb::core::TypedColumnValue::float64("angular_velocity_x", 0.01),
        zeptodb::core::TypedColumnValue::float64("angular_velocity_y", 0.02),
        zeptodb::core::TypedColumnValue::float64("angular_velocity_z", 0.03),
        zeptodb::core::TypedColumnValue::float64("linear_acceleration_x", 9.81),
        zeptodb::core::TypedColumnValue::float64("linear_acceleration_y", 0.0),
        zeptodb::core::TypedColumnValue::float64("linear_acceleration_z", 0.0),
    };

    ASSERT_TRUE(pipeline.ingest_typed_row(std::move(row)));
    EXPECT_TRUE(pipeline.schema_registry().has_data("ros_imu_typed"));

    auto result = ex.execute(
        "SELECT orientation_w, robot_id, frame_id, debug_code FROM ros_imu_typed "
        "WHERE robot_id = " + std::to_string(robot_code));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    ASSERT_EQ(result.typed_rows.size(), 1u);
    ASSERT_EQ(result.typed_rows[0].size(), 4u);
    EXPECT_DOUBLE_EQ(result.typed_rows[0][0].f, 0.707);
    EXPECT_EQ(result.typed_rows[0][1].i, static_cast<int64_t>(robot_code));
    EXPECT_EQ(result.typed_rows[0][2].i, static_cast<int64_t>(frame_code));
    EXPECT_EQ(result.typed_rows[0][3].i, 0);
}

TEST(Ros2ConsumerTest, TypedProfileRemoteRouteForwardsTypedRow) {
    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute(typed_profile_create_sql(
        "ros_imu_remote", Ros2StandardProfile::Imu));
    ASSERT_TRUE(created.ok()) << created.error;
    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_imu_remote");
    ASSERT_NE(tid, 0);

    Ros2Config cfg = typed_profile_config("sensor_msgs/msg/Imu", "ros_imu_remote");
    cfg.backpressure_retries = 0;
    cfg.backpressure_sleep_us = 0;
    Ros2Consumer consumer(cfg);
    consumer.set_pipeline(&pipeline);

    auto router = std::make_shared<zeptodb::cluster::PartitionRouter>();
    router->add_node(1);
    router->add_node(42);
    zeptodb::SymbolId remote_symbol = 0;
    for (zeptodb::SymbolId sym = 1; sym < 10'000; ++sym) {
        if (router->route(tid, sym) == 42) {
            remote_symbol = sym;
            break;
        }
    }
    ASSERT_NE(remote_symbol, 0u);

    auto stub = std::make_shared<CountingRos2RpcClient>();
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes;
    remotes.emplace(42, stub);
    consumer.set_routing(/*local_id=*/1, router, std::move(remotes));

    zeptodb::core::TypedRowMessage row;
    row.table_id = 0;
    row.symbol_id = remote_symbol;
    row.timestamp = 123'000;
    row.columns = {
        zeptodb::core::TypedColumnValue::timestamp("timestamp", 123'000),
        zeptodb::core::TypedColumnValue::timestamp("recv_ts", 123'050),
        zeptodb::core::TypedColumnValue::float64("orientation_w", 0.5),
    };

    EXPECT_TRUE(consumer.on_typed_row("/robot/profile", std::move(row)));
    auto s = consumer.stats();
    EXPECT_EQ(s.route_remote, 1u);
    EXPECT_EQ(s.route_local, 0u);
    EXPECT_EQ(s.rows_ingested, 1u);
    EXPECT_EQ(s.messages_dropped, 0u);
    EXPECT_EQ(s.ingest_failures, 0u);
    EXPECT_EQ(stub->tick_calls.load(), 0);
    EXPECT_EQ(stub->typed_row_calls.load(), 1);
    EXPECT_EQ(stub->last_row.table_id, tid);
    EXPECT_EQ(stub->last_row.symbol_id, remote_symbol);
    EXPECT_EQ(stub->last_row.timestamp, 123'000);
    EXPECT_EQ(stub->last_row.columns.size(), 3u);
    EXPECT_FALSE(pipeline.schema_registry().has_data("ros_imu_remote"));
}

TEST(Ros2ConsumerTest, TypedRowRpcProtocolRoundTripsValues) {
    zeptodb::core::TypedRowMessage row;
    row.table_id = 7;
    row.symbol_id = 42;
    row.timestamp = 123'456;
    row.columns = {
        zeptodb::core::TypedColumnValue::timestamp("timestamp", 123'456),
        zeptodb::core::TypedColumnValue::int32("quality", -2),
        zeptodb::core::TypedColumnValue::float64("orientation_w", 0.707),
        zeptodb::core::TypedColumnValue::symbol("frame_id", 99),
    };

    const auto bytes = zeptodb::cluster::serialize_typed_row(row);
    zeptodb::core::TypedRowMessage out;
    ASSERT_TRUE(zeptodb::cluster::deserialize_typed_row(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.table_id, row.table_id);
    EXPECT_EQ(out.symbol_id, row.symbol_id);
    EXPECT_EQ(out.timestamp, row.timestamp);
    ASSERT_EQ(out.columns.size(), row.columns.size());
    EXPECT_EQ(out.columns[0].name, "timestamp");
    EXPECT_EQ(out.columns[0].type, zeptodb::storage::ColumnType::TIMESTAMP_NS);
    EXPECT_EQ(out.columns[0].i64, 123'456);
    EXPECT_EQ(out.columns[1].type, zeptodb::storage::ColumnType::INT32);
    EXPECT_EQ(out.columns[1].i64, -2);
    EXPECT_EQ(out.columns[2].type, zeptodb::storage::ColumnType::FLOAT64);
    EXPECT_DOUBLE_EQ(out.columns[2].f64, 0.707);
    EXPECT_EQ(out.columns[3].type, zeptodb::storage::ColumnType::SYMBOL);
    EXPECT_EQ(out.columns[3].u32, 99u);
}

TEST(Ros2ConsumerTest, TypedRowIngestRejectsSchemaMismatch) {
    zeptodb::core::ZeptoPipeline pipeline;
    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute("CREATE TABLE ros_bad (timestamp TIMESTAMP_NS, "
                              "orientation_w INT64)");
    ASSERT_TRUE(created.ok()) << created.error;

    zeptodb::core::TypedRowMessage row;
    row.table_id = pipeline.schema_registry().get_table_id("ros_bad");
    row.symbol_id = 700;
    row.timestamp = 1;
    row.columns = {
        zeptodb::core::TypedColumnValue::timestamp("timestamp", 1),
        zeptodb::core::TypedColumnValue::float64("orientation_w", 0.5),
    };

    EXPECT_FALSE(pipeline.ingest_typed_row(std::move(row)));
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

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
TEST(Ros2ConsumerTest, LiveImuSubscriberIngestsStandardProfileIntoTable) {
#if defined(__unix__) || defined(__APPLE__)
    setenv("ROS_DOMAIN_ID", "79", 1);
#endif
    TrialLicenseGuard license;

    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    ex.execute("CREATE TABLE ros_imu_live (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");
    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_imu_live");
    ASSERT_NE(tid, 0);

    Ros2Config cfg;
    cfg.node_name = "zepto_ros2_live_imu_test";
    Ros2SubscriptionConfig sub;
    sub.topic = "/zepto_ros2/live_imu";
    sub.message_type = "sensor_msgs/msg/Imu";
    sub.mode = Ros2IngestMode::StandardProfile;
    sub.table_name = "ros_imu_live";
    sub.queue_capacity = 16;
    sub.drop_on_full = false;
    sub.fields.push_back({"angular_velocity.z", 50, 1000.0});
    sub.fields.push_back({"linear_acceleration.x", 51, 1000.0});
    cfg.subscriptions.push_back(std::move(sub));

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
    auto node = std::make_shared<rclcpp::Node>("zepto_ros2_live_imu_test_pub", node_options);
    auto pub = node->create_publisher<sensor_msgs::msg::Imu>(
        "/zepto_ros2/live_imu", rclcpp::QoS(rclcpp::KeepLast(10)).reliable());

    using namespace std::chrono_literals;
    for (int i = 0; i < 40 && pub->get_subscription_count() == 0; ++i) {
        rclcpp::spin_some(node);
        std::this_thread::sleep_for(50ms);
    }

    sensor_msgs::msg::Imu msg;
    msg.header.stamp.sec = 12;
    msg.header.stamp.nanosec = 345;
    msg.angular_velocity.z = 1.25;
    msg.linear_acceleration.x = 9.81;
    pub->publish(msg);

    bool observed = false;
    Ros2Stats stats;
    for (int i = 0; i < 100; ++i) {
        rclcpp::spin_some(node);
        pipeline.drain_sync(100);

        stats = consumer.stats();
        const auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
        if (stats.messages_consumed >= 1 && stats.rows_ingested >= 2 && !parts.empty()) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    consumer.stop();
    context->shutdown("LiveImuSubscriberIngestsStandardProfileIntoTable done");

    EXPECT_TRUE(observed);
    EXPECT_EQ(stats.messages_consumed, 1u);
    EXPECT_EQ(stats.rows_ingested, 2u);
}

TEST(Ros2ConsumerTest, LiveImuSubscriberIngestsTypedProfileIntoTable) {
#if defined(__unix__) || defined(__APPLE__)
    setenv("ROS_DOMAIN_ID", "80", 1);
#endif
    TrialLicenseGuard license;

    zeptodb::core::PipelineConfig pcfg;
    pcfg.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    zeptodb::core::ZeptoPipeline pipeline(pcfg);

    zeptodb::sql::QueryExecutor ex(pipeline);
    auto created = ex.execute(typed_profile_create_sql(
        "ros_imu_typed_live", Ros2StandardProfile::Imu));
    ASSERT_TRUE(created.ok()) << created.error;

    const uint16_t tid = pipeline.schema_registry().get_table_id("ros_imu_typed_live");
    ASSERT_NE(tid, 0);

    Ros2Config cfg;
    cfg.node_name = "zepto_ros2_live_imu_typed_test";
    cfg.robot_id = "arm-01";
    cfg.session_id = "session-live";
    Ros2SubscriptionConfig sub;
    sub.topic = "/zepto_ros2/live_imu_typed";
    sub.message_type = "sensor_msgs/msg/Imu";
    sub.mode = Ros2IngestMode::TypedProfile;
    sub.table_name = "ros_imu_typed_live";
    sub.typed_partition_symbol_id = 808;
    sub.queue_capacity = 16;
    sub.drop_on_full = false;
    cfg.subscriptions.push_back(std::move(sub));

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
    auto node = std::make_shared<rclcpp::Node>("zepto_ros2_live_imu_typed_test_pub",
                                               node_options);
    auto pub = node->create_publisher<sensor_msgs::msg::Imu>(
        "/zepto_ros2/live_imu_typed", rclcpp::QoS(rclcpp::KeepLast(10)).reliable());

    using namespace std::chrono_literals;
    for (int i = 0; i < 40 && pub->get_subscription_count() == 0; ++i) {
        rclcpp::spin_some(node);
        std::this_thread::sleep_for(50ms);
    }

    sensor_msgs::msg::Imu msg;
    msg.header.stamp.sec = 13;
    msg.header.stamp.nanosec = 500;
    msg.header.frame_id = "base_link";
    msg.orientation.w = 0.5;
    msg.angular_velocity.z = 1.25;
    msg.linear_acceleration.x = 9.81;
    pub->publish(msg);

    bool observed = false;
    Ros2Stats stats;
    zeptodb::sql::QueryResultSet result;
    for (int i = 0; i < 100; ++i) {
        rclcpp::spin_some(node);

        stats = consumer.stats();
        const auto parts = pipeline.partition_manager().get_partitions_for_table(tid);
        result = ex.execute("SELECT orientation_w, linear_acceleration_x, frame_id "
                            "FROM ros_imu_typed_live WHERE symbol = 808");
        if (stats.messages_consumed >= 1 && stats.rows_ingested >= 1 &&
            !parts.empty() && result.ok() && result.rows.size() == 1) {
            observed = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    consumer.stop();
    context->shutdown("LiveImuSubscriberIngestsTypedProfileIntoTable done");

    ASSERT_TRUE(observed) << result.error;
    EXPECT_EQ(stats.messages_consumed, 1u);
    EXPECT_EQ(stats.rows_ingested, 1u);
    ASSERT_EQ(result.typed_rows.size(), 1u);
    EXPECT_DOUBLE_EQ(result.typed_rows[0][0].f, 0.5);
    EXPECT_DOUBLE_EQ(result.typed_rows[0][1].f, 9.81);
    EXPECT_EQ(result.typed_rows[0][2].i,
              pipeline.symbol_dict().find("base_link"));
}
#endif
#endif

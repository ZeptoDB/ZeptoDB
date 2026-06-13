#pragma once
// ============================================================================
// ZeptoDB: ROS 2 Consumer (Physical AI / Robotics)
// ============================================================================
// ROS 2 bridge for routing telemetry samples into ZeptoDB.
//
// MVP direction:
//   ROS 2 topic / rosbag2 -> scalar/profile mapping -> ZeptoPipeline
//
// This file intentionally keeps all ROS 2 runtime types out of the public
// surface. Pure config validation, time conversion, scalar mapping, routing,
// bag config, and stats are unit-testable without a live ROS graph or rclcpp
// dependency. Live subscriber and rosbag2 paths support std_msgs scalar
// messages, scalarized standard Physical AI profiles, and schema-aware typed
// profile rows for IMU, JointState, Odometry, TFMessage, and LaserScan.
// ============================================================================

#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"
#include "zeptodb/common/types.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/ingestion/tick_plant.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zeptodb::feeds {

// ============================================================================
// ROS 2 bridge config
// ============================================================================

enum class Ros2IngestMode : uint8_t {
    ScalarFields,
    StandardProfile,
    TypedProfile,
};

enum class Ros2StandardProfile : uint8_t {
    None,
    Imu,
    JointState,
    Odometry,
    TfMessage,
    LaserScan,
};

enum class Ros2ClockDomain : uint8_t {
    System,
    Steady,
    Sim,
    Ptp,
    Unknown,
};

struct Ros2Time {
    int32_t  sec     = 0;
    uint32_t nanosec = 0;
};

struct Ros2FieldMapping {
    // Field path inside the ROS 2 message. The live std_msgs scalar path
    // requires "data"; standard profiles use paths such as
    // "linear_acceleration.x", "pose.position.x", and "ranges.mean".
    std::string field_path;

    // Engine symbol assigned to this field. 0 is rejected by config validation
    // so telemetry never lands in an ambiguous symbol bucket.
    SymbolId symbol_id = 0;

    // Multiplier applied before writing into TickMessage::price.
    double value_scale = 1.0;
};

struct Ros2SubscriptionConfig {
    std::string topic;
    std::string message_type;
    Ros2IngestMode mode = Ros2IngestMode::ScalarFields;

    // Empty = legacy/default table_id 0. Non-empty is resolved via the
    // connected pipeline's SchemaRegistry in set_pipeline(). TypedProfile
    // requires a non-empty table_name whose schema matches the profile schema.
    std::string table_name;

    std::vector<Ros2FieldMapping> fields;

    // Partition symbol for TypedProfile wide rows. The row still stores
    // robot_id/topic/frame_id/joint_id as typed columns; this value controls
    // ZeptoDB's table-scoped partition key and cluster hash.
    SymbolId typed_partition_symbol_id = 1;

    // Connector-side queue capacity for the live subscriber.
    size_t queue_capacity = 1024;
    bool   drop_on_full   = true;
};

struct Ros2Config {
    std::string node_name  = "zepto_ros2_bridge";
    std::string robot_id;
    std::string session_id;
    Ros2ClockDomain clock_domain = Ros2ClockDomain::Unknown;

    std::vector<Ros2SubscriptionConfig> subscriptions;

    int backpressure_retries  = 3;
    int backpressure_sleep_us = 100;
};

struct Ros2ScalarSample {
    std::string topic;
    SymbolId    symbol_id     = 0;
    int64_t     value         = 0;
    uint64_t    source_ts_ns  = 0;
    uint64_t    recv_ts_ns    = 0;
    uint32_t    quality       = 1;
};

struct Ros2Header {
    uint64_t stamp_ns = 0;
    std::string frame_id;
};

struct Ros2Vector3 {
    long double x = 0.0L;
    long double y = 0.0L;
    long double z = 0.0L;
};

struct Ros2Quaternion {
    long double x = 0.0L;
    long double y = 0.0L;
    long double z = 0.0L;
    long double w = 1.0L;
};

struct Ros2Pose {
    Ros2Vector3 position;
    Ros2Quaternion orientation;
};

struct Ros2Twist {
    Ros2Vector3 linear;
    Ros2Vector3 angular;
};

struct Ros2ImuSample {
    std::string topic;
    Ros2Header header;
    Ros2Quaternion orientation;
    Ros2Vector3 angular_velocity;
    Ros2Vector3 linear_acceleration;
    uint64_t recv_ts_ns = 0;
    uint32_t quality = 1;
};

struct Ros2JointStateSample {
    std::string topic;
    Ros2Header header;
    std::vector<std::string> names;
    std::vector<long double> positions;
    std::vector<long double> velocities;
    std::vector<long double> efforts;
    uint64_t recv_ts_ns = 0;
    uint32_t quality = 1;
};

struct Ros2OdometrySample {
    std::string topic;
    Ros2Header header;
    std::string child_frame_id;
    Ros2Pose pose;
    Ros2Twist twist;
    uint64_t recv_ts_ns = 0;
    uint32_t quality = 1;
};

struct Ros2TransformSample {
    Ros2Header header;
    std::string child_frame_id;
    Ros2Vector3 translation;
    Ros2Quaternion rotation;
};

struct Ros2TfMessageSample {
    std::string topic;
    std::vector<Ros2TransformSample> transforms;
    uint64_t recv_ts_ns = 0;
    uint32_t quality = 1;
};

struct Ros2LaserScanSample {
    std::string topic;
    Ros2Header header;
    long double angle_min = 0.0L;
    long double angle_max = 0.0L;
    long double angle_increment = 0.0L;
    long double time_increment = 0.0L;
    long double scan_time = 0.0L;
    long double range_min = 0.0L;
    long double range_max = 0.0L;
    std::vector<long double> ranges;
    std::vector<long double> intensities;
    uint64_t recv_ts_ns = 0;
    uint32_t quality = 1;
};

struct Ros2BagConfig {
    // Path or URI accepted by rosbag2_cpp::Reader.
    std::string uri;

    // Empty means all topics configured in Ros2Config::subscriptions.
    std::vector<std::string> topics;

    // 0 means read the full bag.
    size_t max_messages = 0;

    // When false, bag topics that are not configured as subscriptions are
    // skipped. When true, the import/replay stops at the first unknown topic.
    bool fail_on_unknown_topic = false;

    // Used by replay_bag(); import_bag() always imports as fast as possible.
    double replay_speed = 1.0;
};

struct Ros2BagStats {
    bool completed = false;
    std::string error;

    uint64_t messages_read     = 0;
    uint64_t messages_consumed = 0;
    uint64_t messages_skipped  = 0;
    uint64_t decode_errors     = 0;
    uint64_t rows_ingested     = 0;

    uint64_t first_source_ts_ns = 0;
    uint64_t last_source_ts_ns  = 0;
};

// ============================================================================
// Ros2Stats: per-bridge statistics snapshot
// ============================================================================

struct Ros2Stats {
    uint64_t messages_consumed = 0;
    uint64_t rows_ingested     = 0;
    uint64_t bytes_consumed    = 0;
    uint64_t decode_errors     = 0;
    uint64_t messages_dropped  = 0;
    uint64_t route_local       = 0;
    uint64_t route_remote      = 0;
    uint64_t ingest_failures   = 0;
    int64_t  last_source_lag_ns = 0;
};

// ============================================================================
// Ros2Consumer
// ============================================================================

class Ros2Consumer {
public:
    explicit Ros2Consumer(Ros2Config config);
    ~Ros2Consumer();

    Ros2Consumer(const Ros2Consumer&)            = delete;
    Ros2Consumer& operator=(const Ros2Consumer&) = delete;

    // Routing setup — call before start() or on_scalar_sample().
    void set_pipeline(zeptodb::core::ZeptoPipeline* pipeline);
    void set_routing(
        zeptodb::cluster::NodeId local_id,
        std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
        std::unordered_map<zeptodb::cluster::NodeId,
            std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes);

    // Lifecycle. start() validates config and feature gates. With
    // ZEPTO_ROS2_AVAILABLE it starts live std_msgs scalar subscriptions and,
    // when standard message packages are available, supported standard
    // profile subscriptions. Otherwise it fail-closes after logging the
    // missing ROS 2 runtime/profile dependency.
    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    Ros2Stats stats() const;

    // rosbag2 import/replay. These methods fail closed unless the target was
    // built with ZEPTO_USE_ROS2=ON and rosbag2_cpp was found by CMake.
    Ros2BagStats import_bag(const Ros2BagConfig& bag_config);
    Ros2BagStats replay_bag(const Ros2BagConfig& bag_config);

    // Testable ingestion hooks.
    bool on_scalar_sample(const Ros2ScalarSample& sample);
    bool on_typed_row(const std::string& topic, zeptodb::core::TypedRowMessage row);
    bool ingest_decoded(zeptodb::ingestion::TickMessage msg);

    // Pure helpers.
    static std::optional<std::string> validate_config(const Ros2Config& config);
    static std::optional<std::string>
    validate_live_scalar_subscription(const Ros2SubscriptionConfig& sub);
    static std::optional<std::string>
    validate_standard_profile_subscription(const Ros2SubscriptionConfig& sub);
    static std::optional<std::string>
    validate_typed_profile_subscription(const Ros2SubscriptionConfig& sub);
    static std::optional<std::string>
    validate_bag_config(const Ros2BagConfig& bag_config);
    static bool is_supported_live_scalar_type(std::string_view message_type) noexcept;
    static Ros2StandardProfile
    standard_profile_for_message_type(std::string_view message_type) noexcept;
    static bool is_supported_standard_profile_type(std::string_view message_type) noexcept;
    static std::vector<zeptodb::storage::ColumnDef>
    typed_profile_schema(Ros2StandardProfile profile);
    static std::optional<uint64_t> time_to_ns(Ros2Time time) noexcept;
    static std::optional<int64_t>
    scale_numeric_value(long double value, long double scale) noexcept;
    static std::optional<zeptodb::ingestion::TickMessage>
    scalar_sample_to_tick(const Ros2ScalarSample& sample);

    // Standard profile flatteners are intentionally ROS-type-free so mapping
    // can be tested without rclcpp. JointState and TF use symbol_id + element
    // index for array expansion; LaserScan emits configured metadata and
    // finite numeric summaries instead of raw range expansion.
    static std::vector<Ros2ScalarSample>
    imu_sample_to_scalars(const Ros2ImuSample& sample,
                          const Ros2SubscriptionConfig& sub);
    static std::vector<Ros2ScalarSample>
    joint_state_sample_to_scalars(const Ros2JointStateSample& sample,
                                  const Ros2SubscriptionConfig& sub);
    static std::vector<Ros2ScalarSample>
    odometry_sample_to_scalars(const Ros2OdometrySample& sample,
                               const Ros2SubscriptionConfig& sub);
    static std::vector<Ros2ScalarSample>
    tf_message_sample_to_scalars(const Ros2TfMessageSample& sample,
                                 const Ros2SubscriptionConfig& sub);
    static std::vector<Ros2ScalarSample>
    laser_scan_sample_to_scalars(const Ros2LaserScanSample& sample,
                                 const Ros2SubscriptionConfig& sub);
    static std::string format_prometheus(const std::string& bridge_name,
                                         const Ros2Stats& stats);

private:
    struct TopicBinding {
        std::string table_name;
        uint16_t table_id = 0;
    };

    TopicBinding binding_for_topic(const std::string& topic) const;
    const Ros2SubscriptionConfig* subscription_for_topic(const std::string& topic) const;
    bool has_topic(const std::string& topic) const;
    void record_decode_error();
    void handle_live_scalar(const std::string& topic,
                            SymbolId symbol_id,
                            long double value,
                            long double scale);
    void handle_standard_scalars(const std::vector<Ros2ScalarSample>& samples);
    void handle_typed_rows(const std::string& topic,
                           std::vector<zeptodb::core::TypedRowMessage> rows);
    bool on_scalar_sample_impl(const Ros2ScalarSample& sample, bool count_message);
    Ros2BagStats consume_bag(const Ros2BagConfig& bag_config, bool replay);

    Ros2Config config_;

    zeptodb::core::ZeptoPipeline* pipeline_ = nullptr;

    zeptodb::cluster::NodeId local_id_ = 0;
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router_;
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes_;

    std::atomic<bool> running_{false};

    mutable std::mutex stats_mu_;
    Ros2Stats stats_;

    std::unordered_map<std::string, TopicBinding> topic_bindings_;

    // Opaque rclcpp runtime (Ros2Runtime*) when ZEPTO_ROS2_AVAILABLE.
    void* runtime_handle_ = nullptr;
};

} // namespace zeptodb::feeds

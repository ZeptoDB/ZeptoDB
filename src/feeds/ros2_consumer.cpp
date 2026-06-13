// ============================================================================
// ZeptoDB: ROS 2 Consumer — bridge implementation
// ============================================================================

#include "zeptodb/feeds/ros2_consumer.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

#ifdef ZEPTO_ROS2_AVAILABLE
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/u_int32.hpp>
#include <std_msgs/msg/u_int64.hpp>
#endif

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <tf2_msgs/msg/tf_message.hpp>
#endif

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_storage/storage_filter.hpp>
#include <rosbag2_storage/storage_interfaces/base_read_interface.hpp>
#endif

namespace zeptodb::feeds {

namespace {

constexpr uint32_t kNanosecondsPerSecond = 1'000'000'000u;

std::optional<SymbolId> symbol_with_offset(SymbolId base, size_t offset) noexcept {
    if (base == 0) {
        return std::nullopt;
    }
    if (offset > static_cast<size_t>(std::numeric_limits<SymbolId>::max() - base)) {
        return std::nullopt;
    }
    return static_cast<SymbolId>(base + static_cast<SymbolId>(offset));
}

bool imu_field_supported(std::string_view field) noexcept {
    return field == "orientation.x" ||
           field == "orientation.y" ||
           field == "orientation.z" ||
           field == "orientation.w" ||
           field == "angular_velocity.x" ||
           field == "angular_velocity.y" ||
           field == "angular_velocity.z" ||
           field == "linear_acceleration.x" ||
           field == "linear_acceleration.y" ||
           field == "linear_acceleration.z";
}

bool joint_state_field_supported(std::string_view field) noexcept {
    return field == "position" || field == "velocity" || field == "effort";
}

bool odometry_field_supported(std::string_view field) noexcept {
    return field == "pose.position.x" ||
           field == "pose.position.y" ||
           field == "pose.position.z" ||
           field == "pose.orientation.x" ||
           field == "pose.orientation.y" ||
           field == "pose.orientation.z" ||
           field == "pose.orientation.w" ||
           field == "twist.linear.x" ||
           field == "twist.linear.y" ||
           field == "twist.linear.z" ||
           field == "twist.angular.x" ||
           field == "twist.angular.y" ||
           field == "twist.angular.z";
}

bool tf_field_supported(std::string_view field) noexcept {
    return field == "translation.x" ||
           field == "translation.y" ||
           field == "translation.z" ||
           field == "rotation.x" ||
           field == "rotation.y" ||
           field == "rotation.z" ||
           field == "rotation.w";
}

bool laser_scan_field_supported(std::string_view field) noexcept {
    return field == "angle_min" ||
           field == "angle_max" ||
           field == "angle_increment" ||
           field == "time_increment" ||
           field == "scan_time" ||
           field == "range_min" ||
           field == "range_max" ||
           field == "ranges.count" ||
           field == "ranges.min" ||
           field == "ranges.max" ||
           field == "ranges.mean" ||
           field == "intensities.count" ||
           field == "intensities.min" ||
           field == "intensities.max" ||
           field == "intensities.mean";
}

bool profile_field_supported(Ros2StandardProfile profile, std::string_view field) noexcept {
    switch (profile) {
    case Ros2StandardProfile::Imu:
        return imu_field_supported(field);
    case Ros2StandardProfile::JointState:
        return joint_state_field_supported(field);
    case Ros2StandardProfile::Odometry:
        return odometry_field_supported(field);
    case Ros2StandardProfile::TfMessage:
        return tf_field_supported(field);
    case Ros2StandardProfile::LaserScan:
        return laser_scan_field_supported(field);
    case Ros2StandardProfile::None:
        return false;
    }
    return false;
}

std::vector<zeptodb::storage::ColumnDef> common_typed_schema() {
    using zeptodb::storage::ColumnDef;
    using zeptodb::storage::ColumnType;
    return {
        {"timestamp", ColumnType::TIMESTAMP_NS},
        {"recv_ts", ColumnType::TIMESTAMP_NS},
        {"robot_id", ColumnType::SYMBOL},
        {"session_id", ColumnType::SYMBOL},
        {"topic", ColumnType::SYMBOL},
        {"frame_id", ColumnType::SYMBOL},
        {"quality", ColumnType::INT32},
    };
}

void append_schema(std::vector<zeptodb::storage::ColumnDef>& schema,
                   std::initializer_list<zeptodb::storage::ColumnDef> columns) {
    schema.insert(schema.end(), columns.begin(), columns.end());
}

std::optional<std::string> validate_typed_table_schema(
    const zeptodb::storage::TableSchema& table,
    Ros2StandardProfile profile) {
    const auto expected = Ros2Consumer::typed_profile_schema(profile);
    for (const auto& required : expected) {
        const auto it = std::find_if(
            table.columns.begin(),
            table.columns.end(),
            [&](const zeptodb::storage::ColumnDef& actual) {
                return actual.name == required.name;
            });
        if (it == table.columns.end()) {
            return "typed profile table '" + table.table_name +
                   "' missing required column: " + required.name;
        }
        if (it->type != required.type) {
            return "typed profile table '" + table.table_name +
                   "' has wrong type for column: " + required.name;
        }
    }
    return std::nullopt;
}

std::optional<long double>
imu_field_value(const Ros2ImuSample& sample, std::string_view field) noexcept {
    if (field == "orientation.x") return sample.orientation.x;
    if (field == "orientation.y") return sample.orientation.y;
    if (field == "orientation.z") return sample.orientation.z;
    if (field == "orientation.w") return sample.orientation.w;
    if (field == "angular_velocity.x") return sample.angular_velocity.x;
    if (field == "angular_velocity.y") return sample.angular_velocity.y;
    if (field == "angular_velocity.z") return sample.angular_velocity.z;
    if (field == "linear_acceleration.x") return sample.linear_acceleration.x;
    if (field == "linear_acceleration.y") return sample.linear_acceleration.y;
    if (field == "linear_acceleration.z") return sample.linear_acceleration.z;
    return std::nullopt;
}

std::optional<long double>
odometry_field_value(const Ros2OdometrySample& sample, std::string_view field) noexcept {
    if (field == "pose.position.x") return sample.pose.position.x;
    if (field == "pose.position.y") return sample.pose.position.y;
    if (field == "pose.position.z") return sample.pose.position.z;
    if (field == "pose.orientation.x") return sample.pose.orientation.x;
    if (field == "pose.orientation.y") return sample.pose.orientation.y;
    if (field == "pose.orientation.z") return sample.pose.orientation.z;
    if (field == "pose.orientation.w") return sample.pose.orientation.w;
    if (field == "twist.linear.x") return sample.twist.linear.x;
    if (field == "twist.linear.y") return sample.twist.linear.y;
    if (field == "twist.linear.z") return sample.twist.linear.z;
    if (field == "twist.angular.x") return sample.twist.angular.x;
    if (field == "twist.angular.y") return sample.twist.angular.y;
    if (field == "twist.angular.z") return sample.twist.angular.z;
    return std::nullopt;
}

std::optional<long double>
tf_field_value(const Ros2TransformSample& sample, std::string_view field) noexcept {
    if (field == "translation.x") return sample.translation.x;
    if (field == "translation.y") return sample.translation.y;
    if (field == "translation.z") return sample.translation.z;
    if (field == "rotation.x") return sample.rotation.x;
    if (field == "rotation.y") return sample.rotation.y;
    if (field == "rotation.z") return sample.rotation.z;
    if (field == "rotation.w") return sample.rotation.w;
    return std::nullopt;
}

struct NumericSummary {
    size_t count = 0;
    long double min = 0.0L;
    long double max = 0.0L;
    long double mean = 0.0L;
    bool has_finite = false;
};

NumericSummary summarize_finite(const std::vector<long double>& values) noexcept {
    NumericSummary summary;
    summary.count = values.size();
    long double sum = 0.0L;
    size_t finite_count = 0;
    for (const auto value : values) {
        if (!std::isfinite(value)) {
            continue;
        }
        if (!summary.has_finite) {
            summary.min = value;
            summary.max = value;
            summary.has_finite = true;
        } else {
            summary.min = std::min(summary.min, value);
            summary.max = std::max(summary.max, value);
        }
        sum += value;
        finite_count++;
    }
    if (finite_count != 0) {
        summary.mean = sum / static_cast<long double>(finite_count);
    }
    return summary;
}

std::optional<long double> laser_scan_field_value(
    const Ros2LaserScanSample& sample,
    const NumericSummary& ranges,
    const NumericSummary& intensities,
    std::string_view field) noexcept {
    if (field == "angle_min") return sample.angle_min;
    if (field == "angle_max") return sample.angle_max;
    if (field == "angle_increment") return sample.angle_increment;
    if (field == "time_increment") return sample.time_increment;
    if (field == "scan_time") return sample.scan_time;
    if (field == "range_min") return sample.range_min;
    if (field == "range_max") return sample.range_max;
    if (field == "ranges.count") return static_cast<long double>(ranges.count);
    if (field == "ranges.min") return ranges.has_finite ? std::optional<long double>{ranges.min} : std::nullopt;
    if (field == "ranges.max") return ranges.has_finite ? std::optional<long double>{ranges.max} : std::nullopt;
    if (field == "ranges.mean") return ranges.has_finite ? std::optional<long double>{ranges.mean} : std::nullopt;
    if (field == "intensities.count") return static_cast<long double>(intensities.count);
    if (field == "intensities.min") return intensities.has_finite ? std::optional<long double>{intensities.min} : std::nullopt;
    if (field == "intensities.max") return intensities.has_finite ? std::optional<long double>{intensities.max} : std::nullopt;
    if (field == "intensities.mean") return intensities.has_finite ? std::optional<long double>{intensities.mean} : std::nullopt;
    return std::nullopt;
}

void append_scalar_if_valid(std::vector<Ros2ScalarSample>& out,
                            const std::string& topic,
                            const Ros2FieldMapping& field,
                            long double value,
                            uint64_t source_ts_ns,
                            uint64_t recv_ts_ns,
                            uint32_t quality,
                            size_t symbol_offset = 0) {
    const auto symbol = symbol_with_offset(field.symbol_id, symbol_offset);
    if (!symbol) {
        return;
    }
    const auto scaled = Ros2Consumer::scale_numeric_value(
        value, static_cast<long double>(field.value_scale));
    if (!scaled) {
        return;
    }

    Ros2ScalarSample sample;
    sample.topic = topic;
    sample.symbol_id = *symbol;
    sample.value = *scaled;
    sample.source_ts_ns = source_ts_ns;
    sample.recv_ts_ns = recv_ts_ns;
    sample.quality = quality;
    out.push_back(std::move(sample));
}

uint32_t intern_symbol_or_zero(zeptodb::storage::StringDictionary& dict,
                               const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    return dict.intern(value);
}

uint64_t source_or_recv_ts(uint64_t source_ts_ns, uint64_t recv_ts_ns) noexcept {
    return source_ts_ns != 0 ? source_ts_ns : recv_ts_ns;
}

void append_common_typed_columns(
    std::vector<zeptodb::core::TypedColumnValue>& columns,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id,
    const std::string& topic,
    const Ros2Header& header,
    uint64_t recv_ts_ns,
    uint32_t quality) {
    const uint64_t ts = source_or_recv_ts(header.stamp_ns, recv_ts_ns);
    columns.push_back(zeptodb::core::TypedColumnValue::timestamp(
        "timestamp", static_cast<int64_t>(ts)));
    columns.push_back(zeptodb::core::TypedColumnValue::timestamp(
        "recv_ts", static_cast<int64_t>(recv_ts_ns)));
    columns.push_back(zeptodb::core::TypedColumnValue::symbol(
        "robot_id", intern_symbol_or_zero(dict, robot_id)));
    columns.push_back(zeptodb::core::TypedColumnValue::symbol(
        "session_id", intern_symbol_or_zero(dict, session_id)));
    columns.push_back(zeptodb::core::TypedColumnValue::symbol(
        "topic", intern_symbol_or_zero(dict, topic)));
    columns.push_back(zeptodb::core::TypedColumnValue::symbol(
        "frame_id", intern_symbol_or_zero(dict, header.frame_id)));
    columns.push_back(zeptodb::core::TypedColumnValue::int32(
        "quality", static_cast<int32_t>(quality)));
}

void append_vector3_columns(std::vector<zeptodb::core::TypedColumnValue>& columns,
                            std::string_view prefix,
                            const Ros2Vector3& value) {
    const std::string base(prefix);
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_x",
                                                              static_cast<double>(value.x)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_y",
                                                              static_cast<double>(value.y)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_z",
                                                              static_cast<double>(value.z)));
}

void append_quaternion_columns(std::vector<zeptodb::core::TypedColumnValue>& columns,
                               std::string_view prefix,
                               const Ros2Quaternion& value) {
    const std::string base(prefix);
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_x",
                                                              static_cast<double>(value.x)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_y",
                                                              static_cast<double>(value.y)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_z",
                                                              static_cast<double>(value.z)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(base + "_w",
                                                              static_cast<double>(value.w)));
}

zeptodb::core::TypedRowMessage make_typed_row(
    SymbolId partition_symbol_id,
    uint64_t timestamp_ns,
    std::vector<zeptodb::core::TypedColumnValue> columns) {
    zeptodb::core::TypedRowMessage row;
    row.symbol_id = partition_symbol_id;
    row.timestamp = static_cast<Timestamp>(timestamp_ns);
    row.columns = std::move(columns);
    return row;
}

size_t typed_row_payload_bytes(const zeptodb::core::TypedRowMessage& row) {
    size_t bytes = 0;
    for (const auto& column : row.columns) {
        bytes += zeptodb::storage::column_type_size(column.type);
    }
    return bytes;
}

uint64_t typed_row_recv_ts(const zeptodb::core::TypedRowMessage& row) noexcept {
    for (const auto& column : row.columns) {
        if (column.name == "recv_ts" &&
            column.type == zeptodb::storage::ColumnType::TIMESTAMP_NS &&
            column.i64 > 0) {
            return static_cast<uint64_t>(column.i64);
        }
    }
    return 0;
}

[[maybe_unused]] std::vector<zeptodb::core::TypedRowMessage> imu_typed_rows(
    const Ros2ImuSample& sample,
    const Ros2SubscriptionConfig& sub,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    std::vector<zeptodb::core::TypedColumnValue> columns;
    columns.reserve(17);
    append_common_typed_columns(columns, dict, robot_id, session_id, sample.topic,
                                sample.header, sample.recv_ts_ns, sample.quality);
    append_quaternion_columns(columns, "orientation", sample.orientation);
    append_vector3_columns(columns, "angular_velocity", sample.angular_velocity);
    append_vector3_columns(columns, "linear_acceleration", sample.linear_acceleration);

    return {make_typed_row(sub.typed_partition_symbol_id,
                           source_or_recv_ts(sample.header.stamp_ns, sample.recv_ts_ns),
                           std::move(columns))};
}

[[maybe_unused]] std::vector<zeptodb::core::TypedRowMessage> joint_state_typed_rows(
    const Ros2JointStateSample& sample,
    const Ros2SubscriptionConfig& sub,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    const size_t joint_count = std::max(
        sample.names.size(),
        std::max(sample.positions.size(), std::max(sample.velocities.size(), sample.efforts.size())));

    std::vector<zeptodb::core::TypedRowMessage> rows;
    rows.reserve(joint_count);
    for (size_t i = 0; i < joint_count; ++i) {
        std::vector<zeptodb::core::TypedColumnValue> columns;
        columns.reserve(11);
        append_common_typed_columns(columns, dict, robot_id, session_id, sample.topic,
                                    sample.header, sample.recv_ts_ns, sample.quality);
        const std::string joint_name = i < sample.names.size() ? sample.names[i] : "";
        columns.push_back(zeptodb::core::TypedColumnValue::symbol(
            "joint_id", intern_symbol_or_zero(dict, joint_name)));
        columns.push_back(zeptodb::core::TypedColumnValue::float64(
            "position", i < sample.positions.size() ? static_cast<double>(sample.positions[i]) : 0.0));
        columns.push_back(zeptodb::core::TypedColumnValue::float64(
            "velocity", i < sample.velocities.size() ? static_cast<double>(sample.velocities[i]) : 0.0));
        columns.push_back(zeptodb::core::TypedColumnValue::float64(
            "effort", i < sample.efforts.size() ? static_cast<double>(sample.efforts[i]) : 0.0));
        rows.push_back(make_typed_row(
            sub.typed_partition_symbol_id,
            source_or_recv_ts(sample.header.stamp_ns, sample.recv_ts_ns),
            std::move(columns)));
    }
    return rows;
}

[[maybe_unused]] std::vector<zeptodb::core::TypedRowMessage> odometry_typed_rows(
    const Ros2OdometrySample& sample,
    const Ros2SubscriptionConfig& sub,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    std::vector<zeptodb::core::TypedColumnValue> columns;
    columns.reserve(22);
    append_common_typed_columns(columns, dict, robot_id, session_id, sample.topic,
                                sample.header, sample.recv_ts_ns, sample.quality);
    columns.push_back(zeptodb::core::TypedColumnValue::symbol(
        "child_frame_id", intern_symbol_or_zero(dict, sample.child_frame_id)));
    append_vector3_columns(columns, "pose_position", sample.pose.position);
    append_quaternion_columns(columns, "pose_orientation", sample.pose.orientation);
    append_vector3_columns(columns, "twist_linear", sample.twist.linear);
    append_vector3_columns(columns, "twist_angular", sample.twist.angular);

    return {make_typed_row(sub.typed_partition_symbol_id,
                           source_or_recv_ts(sample.header.stamp_ns, sample.recv_ts_ns),
                           std::move(columns))};
}

[[maybe_unused]] std::vector<zeptodb::core::TypedRowMessage> tf_message_typed_rows(
    const Ros2TfMessageSample& sample,
    const Ros2SubscriptionConfig& sub,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    std::vector<zeptodb::core::TypedRowMessage> rows;
    rows.reserve(sample.transforms.size());
    for (const auto& transform : sample.transforms) {
        std::vector<zeptodb::core::TypedColumnValue> columns;
        columns.reserve(16);
        append_common_typed_columns(columns, dict, robot_id, session_id, sample.topic,
                                    transform.header, sample.recv_ts_ns, sample.quality);
        columns.push_back(zeptodb::core::TypedColumnValue::symbol(
            "child_frame_id", intern_symbol_or_zero(dict, transform.child_frame_id)));
        append_vector3_columns(columns, "translation", transform.translation);
        append_quaternion_columns(columns, "rotation", transform.rotation);
        rows.push_back(make_typed_row(
            sub.typed_partition_symbol_id,
            source_or_recv_ts(transform.header.stamp_ns, sample.recv_ts_ns),
            std::move(columns)));
    }
    return rows;
}

[[maybe_unused]] std::vector<zeptodb::core::TypedRowMessage> laser_scan_typed_rows(
    const Ros2LaserScanSample& sample,
    const Ros2SubscriptionConfig& sub,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    const auto ranges = summarize_finite(sample.ranges);
    const auto intensities = summarize_finite(sample.intensities);

    std::vector<zeptodb::core::TypedColumnValue> columns;
    columns.reserve(23);
    append_common_typed_columns(columns, dict, robot_id, session_id, sample.topic,
                                sample.header, sample.recv_ts_ns, sample.quality);
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "angle_min", static_cast<double>(sample.angle_min)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "angle_max", static_cast<double>(sample.angle_max)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "angle_increment", static_cast<double>(sample.angle_increment)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "time_increment", static_cast<double>(sample.time_increment)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "scan_time", static_cast<double>(sample.scan_time)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "range_min", static_cast<double>(sample.range_min)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "range_max", static_cast<double>(sample.range_max)));
    columns.push_back(zeptodb::core::TypedColumnValue::int64(
        "ranges_count", static_cast<int64_t>(ranges.count)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "ranges_min", ranges.has_finite ? static_cast<double>(ranges.min) : 0.0));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "ranges_max", ranges.has_finite ? static_cast<double>(ranges.max) : 0.0));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "ranges_mean", ranges.has_finite ? static_cast<double>(ranges.mean) : 0.0));
    columns.push_back(zeptodb::core::TypedColumnValue::int64(
        "intensities_count", static_cast<int64_t>(intensities.count)));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "intensities_min", intensities.has_finite ? static_cast<double>(intensities.min) : 0.0));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "intensities_max", intensities.has_finite ? static_cast<double>(intensities.max) : 0.0));
    columns.push_back(zeptodb::core::TypedColumnValue::float64(
        "intensities_mean", intensities.has_finite ? static_cast<double>(intensities.mean) : 0.0));

    return {make_typed_row(sub.typed_partition_symbol_id,
                           source_or_recv_ts(sample.header.stamp_ns, sample.recv_ts_ns),
                           std::move(columns))};
}

template <typename Fn>
bool try_with_backpressure(Fn ingest_fn, int retries, int sleep_us) {
    for (int i = 0; i <= retries; ++i) {
        if (ingest_fn()) return true;
        if (i < retries) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }
    return false;
}

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
uint64_t positive_timestamp_ns(rcutils_time_point_value_t ts) noexcept {
    return ts > 0 ? static_cast<uint64_t>(ts) : 0;
}

template <typename MessageT>
std::optional<long double>
decode_rosbag_scalar_message(const rosbag2_storage::SerializedBagMessage& bag_msg) {
    if (!bag_msg.serialized_data) {
        return std::nullopt;
    }

    try {
        rclcpp::SerializedMessage serialized(*bag_msg.serialized_data);
        MessageT msg;
        rclcpp::Serialization<MessageT> serializer;
        serializer.deserialize_message(&serialized, &msg);
        return static_cast<long double>(msg.data);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<long double>
decode_rosbag_scalar_value(const std::string& message_type,
                           const rosbag2_storage::SerializedBagMessage& bag_msg) {
    if (message_type == "std_msgs/msg/Float64") {
        return decode_rosbag_scalar_message<std_msgs::msg::Float64>(bag_msg);
    }
    if (message_type == "std_msgs/msg/Float32") {
        return decode_rosbag_scalar_message<std_msgs::msg::Float32>(bag_msg);
    }
    if (message_type == "std_msgs/msg/Int64") {
        return decode_rosbag_scalar_message<std_msgs::msg::Int64>(bag_msg);
    }
    if (message_type == "std_msgs/msg/Int32") {
        return decode_rosbag_scalar_message<std_msgs::msg::Int32>(bag_msg);
    }
    if (message_type == "std_msgs/msg/UInt64") {
        return decode_rosbag_scalar_message<std_msgs::msg::UInt64>(bag_msg);
    }
    if (message_type == "std_msgs/msg/UInt32") {
        return decode_rosbag_scalar_message<std_msgs::msg::UInt32>(bag_msg);
    }
    return std::nullopt;
}

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
Ros2ImuSample imu_from_ros(const sensor_msgs::msg::Imu& msg, const std::string& topic);
Ros2JointStateSample joint_state_from_ros(
    const sensor_msgs::msg::JointState& msg,
    const std::string& topic);
Ros2OdometrySample odometry_from_ros(
    const nav_msgs::msg::Odometry& msg,
    const std::string& topic);
Ros2TfMessageSample tf_message_from_ros(
    const tf2_msgs::msg::TFMessage& msg,
    const std::string& topic);
Ros2LaserScanSample laser_scan_from_ros(
    const sensor_msgs::msg::LaserScan& msg,
    const std::string& topic);

template <typename MessageT>
std::optional<MessageT>
decode_rosbag_message(const rosbag2_storage::SerializedBagMessage& bag_msg) {
    if (!bag_msg.serialized_data) {
        return std::nullopt;
    }

    try {
        rclcpp::SerializedMessage serialized(*bag_msg.serialized_data);
        MessageT msg;
        rclcpp::Serialization<MessageT> serializer;
        serializer.deserialize_message(&serialized, &msg);
        return msg;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<Ros2ScalarSample> decode_rosbag_standard_profile_scalars(
    const Ros2SubscriptionConfig& sub,
    const rosbag2_storage::SerializedBagMessage& bag_msg,
    uint64_t source_ts_ns,
    uint64_t recv_ts_ns) {
    const auto profile = Ros2Consumer::standard_profile_for_message_type(sub.message_type);
    if (profile == Ros2StandardProfile::Imu) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::Imu>(bag_msg);
        if (!msg) return {};
        auto sample = imu_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return Ros2Consumer::imu_sample_to_scalars(sample, sub);
    }
    if (profile == Ros2StandardProfile::JointState) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::JointState>(bag_msg);
        if (!msg) return {};
        auto sample = joint_state_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return Ros2Consumer::joint_state_sample_to_scalars(sample, sub);
    }
    if (profile == Ros2StandardProfile::Odometry) {
        const auto msg = decode_rosbag_message<nav_msgs::msg::Odometry>(bag_msg);
        if (!msg) return {};
        auto sample = odometry_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return Ros2Consumer::odometry_sample_to_scalars(sample, sub);
    }
    if (profile == Ros2StandardProfile::TfMessage) {
        const auto msg = decode_rosbag_message<tf2_msgs::msg::TFMessage>(bag_msg);
        if (!msg) return {};
        auto sample = tf_message_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        for (auto& transform : sample.transforms) {
            if (transform.header.stamp_ns == 0) {
                transform.header.stamp_ns = source_ts_ns;
            }
        }
        return Ros2Consumer::tf_message_sample_to_scalars(sample, sub);
    }
    if (profile == Ros2StandardProfile::LaserScan) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::LaserScan>(bag_msg);
        if (!msg) return {};
        auto sample = laser_scan_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return Ros2Consumer::laser_scan_sample_to_scalars(sample, sub);
    }
    return {};
}

std::vector<zeptodb::core::TypedRowMessage> decode_rosbag_typed_profile_rows(
    const Ros2SubscriptionConfig& sub,
    const rosbag2_storage::SerializedBagMessage& bag_msg,
    uint64_t source_ts_ns,
    uint64_t recv_ts_ns,
    zeptodb::storage::StringDictionary& dict,
    const std::string& robot_id,
    const std::string& session_id) {
    const auto profile = Ros2Consumer::standard_profile_for_message_type(sub.message_type);
    if (profile == Ros2StandardProfile::Imu) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::Imu>(bag_msg);
        if (!msg) return {};
        auto sample = imu_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return imu_typed_rows(sample, sub, dict, robot_id, session_id);
    }
    if (profile == Ros2StandardProfile::JointState) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::JointState>(bag_msg);
        if (!msg) return {};
        auto sample = joint_state_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return joint_state_typed_rows(sample, sub, dict, robot_id, session_id);
    }
    if (profile == Ros2StandardProfile::Odometry) {
        const auto msg = decode_rosbag_message<nav_msgs::msg::Odometry>(bag_msg);
        if (!msg) return {};
        auto sample = odometry_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return odometry_typed_rows(sample, sub, dict, robot_id, session_id);
    }
    if (profile == Ros2StandardProfile::TfMessage) {
        const auto msg = decode_rosbag_message<tf2_msgs::msg::TFMessage>(bag_msg);
        if (!msg) return {};
        auto sample = tf_message_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        for (auto& transform : sample.transforms) {
            if (transform.header.stamp_ns == 0) {
                transform.header.stamp_ns = source_ts_ns;
            }
        }
        return tf_message_typed_rows(sample, sub, dict, robot_id, session_id);
    }
    if (profile == Ros2StandardProfile::LaserScan) {
        const auto msg = decode_rosbag_message<sensor_msgs::msg::LaserScan>(bag_msg);
        if (!msg) return {};
        auto sample = laser_scan_from_ros(*msg, bag_msg.topic_name);
        sample.recv_ts_ns = recv_ts_ns;
        if (sample.header.stamp_ns == 0) sample.header.stamp_ns = source_ts_ns;
        return laser_scan_typed_rows(sample, sub, dict, robot_id, session_id);
    }
    return {};
}
#endif
#endif

} // namespace

#ifdef ZEPTO_ROS2_AVAILABLE
namespace {

struct Ros2Runtime {
    rclcpp::Context::SharedPtr context;
    rclcpp::Node::SharedPtr node;
    std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
    std::vector<rclcpp::SubscriptionBase::SharedPtr> subscriptions;
    std::thread spin_thread;
};

} // namespace
#endif

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
namespace {

uint64_t now_system_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

template <typename RosTimeT>
uint64_t ros_stamp_to_ns(const RosTimeT& stamp) noexcept {
    return Ros2Consumer::time_to_ns(Ros2Time{stamp.sec, stamp.nanosec}).value_or(0);
}

template <typename RosHeaderT>
Ros2Header header_from_ros(const RosHeaderT& header) {
    Ros2Header out;
    out.stamp_ns = ros_stamp_to_ns(header.stamp);
    out.frame_id = header.frame_id;
    return out;
}

template <typename RosVectorT>
Ros2Vector3 vector3_from_ros(const RosVectorT& value) noexcept {
    return Ros2Vector3{
        static_cast<long double>(value.x),
        static_cast<long double>(value.y),
        static_cast<long double>(value.z),
    };
}

template <typename RosQuaternionT>
Ros2Quaternion quaternion_from_ros(const RosQuaternionT& value) noexcept {
    return Ros2Quaternion{
        static_cast<long double>(value.x),
        static_cast<long double>(value.y),
        static_cast<long double>(value.z),
        static_cast<long double>(value.w),
    };
}

template <typename FloatVectorT>
std::vector<long double> to_long_double_vector(const FloatVectorT& values) {
    std::vector<long double> out;
    out.reserve(values.size());
    for (const auto value : values) {
        out.push_back(static_cast<long double>(value));
    }
    return out;
}

Ros2ImuSample imu_from_ros(const sensor_msgs::msg::Imu& msg, const std::string& topic) {
    Ros2ImuSample sample;
    sample.topic = topic;
    sample.header = header_from_ros(msg.header);
    sample.orientation = quaternion_from_ros(msg.orientation);
    sample.angular_velocity = vector3_from_ros(msg.angular_velocity);
    sample.linear_acceleration = vector3_from_ros(msg.linear_acceleration);
    sample.recv_ts_ns = now_system_ns();
    return sample;
}

Ros2JointStateSample joint_state_from_ros(
    const sensor_msgs::msg::JointState& msg,
    const std::string& topic) {
    Ros2JointStateSample sample;
    sample.topic = topic;
    sample.header = header_from_ros(msg.header);
    sample.names = msg.name;
    sample.positions = to_long_double_vector(msg.position);
    sample.velocities = to_long_double_vector(msg.velocity);
    sample.efforts = to_long_double_vector(msg.effort);
    sample.recv_ts_ns = now_system_ns();
    return sample;
}

Ros2OdometrySample odometry_from_ros(
    const nav_msgs::msg::Odometry& msg,
    const std::string& topic) {
    Ros2OdometrySample sample;
    sample.topic = topic;
    sample.header = header_from_ros(msg.header);
    sample.child_frame_id = msg.child_frame_id;
    sample.pose.position = vector3_from_ros(msg.pose.pose.position);
    sample.pose.orientation = quaternion_from_ros(msg.pose.pose.orientation);
    sample.twist.linear = vector3_from_ros(msg.twist.twist.linear);
    sample.twist.angular = vector3_from_ros(msg.twist.twist.angular);
    sample.recv_ts_ns = now_system_ns();
    return sample;
}

Ros2TfMessageSample tf_message_from_ros(
    const tf2_msgs::msg::TFMessage& msg,
    const std::string& topic) {
    Ros2TfMessageSample sample;
    sample.topic = topic;
    sample.recv_ts_ns = now_system_ns();
    sample.transforms.reserve(msg.transforms.size());
    for (const auto& transform : msg.transforms) {
        Ros2TransformSample mapped;
        mapped.header = header_from_ros(transform.header);
        mapped.child_frame_id = transform.child_frame_id;
        mapped.translation = vector3_from_ros(transform.transform.translation);
        mapped.rotation = quaternion_from_ros(transform.transform.rotation);
        sample.transforms.push_back(std::move(mapped));
    }
    return sample;
}

Ros2LaserScanSample laser_scan_from_ros(
    const sensor_msgs::msg::LaserScan& msg,
    const std::string& topic) {
    Ros2LaserScanSample sample;
    sample.topic = topic;
    sample.header = header_from_ros(msg.header);
    sample.angle_min = static_cast<long double>(msg.angle_min);
    sample.angle_max = static_cast<long double>(msg.angle_max);
    sample.angle_increment = static_cast<long double>(msg.angle_increment);
    sample.time_increment = static_cast<long double>(msg.time_increment);
    sample.scan_time = static_cast<long double>(msg.scan_time);
    sample.range_min = static_cast<long double>(msg.range_min);
    sample.range_max = static_cast<long double>(msg.range_max);
    sample.ranges = to_long_double_vector(msg.ranges);
    sample.intensities = to_long_double_vector(msg.intensities);
    sample.recv_ts_ns = now_system_ns();
    return sample;
}

} // namespace
#endif

// ============================================================================
// Constructor / Destructor
// ============================================================================

Ros2Consumer::Ros2Consumer(Ros2Config config)
    : config_(std::move(config)) {
    topic_bindings_.reserve(config_.subscriptions.size());
    for (const auto& sub : config_.subscriptions) {
        topic_bindings_.emplace(sub.topic, TopicBinding{sub.table_name, 0});
    }
}

Ros2Consumer::~Ros2Consumer() {
    stop();
}

// ============================================================================
// Routing setup
// ============================================================================

void Ros2Consumer::set_pipeline(zeptodb::core::ZeptoPipeline* pipeline) {
    pipeline_ = pipeline;

    for (auto& [topic, binding] : topic_bindings_) {
        binding.table_id = 0;
        if (pipeline_ && !binding.table_name.empty()) {
            const uint16_t tid = pipeline_->schema_registry().get_table_id(binding.table_name);
            if (tid == 0) {
                ZEPTO_ERROR("Ros2Consumer: unknown table '{}' — topic '{}' rows will be dropped",
                            binding.table_name, topic);
                continue;
            }
            const auto* sub = subscription_for_topic(topic);
            if (sub && sub->mode == Ros2IngestMode::TypedProfile) {
                const auto schema = pipeline_->schema_registry().get(binding.table_name);
                if (!schema) {
                    ZEPTO_ERROR("Ros2Consumer: missing schema for typed profile table '{}' "
                                "— topic '{}' rows will be dropped",
                                binding.table_name, topic);
                    continue;
                }
                const auto profile = standard_profile_for_message_type(sub->message_type);
                if (const auto err = validate_typed_table_schema(*schema, profile)) {
                    ZEPTO_ERROR("Ros2Consumer: {} — topic '{}' rows will be dropped",
                                *err, topic);
                    continue;
                }
            }
            binding.table_id = tid;
        }
    }
}

void Ros2Consumer::set_routing(
    zeptodb::cluster::NodeId local_id,
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::RpcClientBase>> remotes)
{
    local_id_ = local_id;
    router_   = std::move(router);
    remotes_  = std::move(remotes);
}

// ============================================================================
// Validation and pure mapping helpers
// ============================================================================

std::optional<std::string> Ros2Consumer::validate_config(const Ros2Config& config) {
    if (config.node_name.empty()) {
        return "node_name must not be empty";
    }
    if (config.subscriptions.empty()) {
        return "at least one subscription is required";
    }
    if (config.backpressure_retries < 0) {
        return "backpressure_retries must be >= 0";
    }
    if (config.backpressure_sleep_us < 0) {
        return "backpressure_sleep_us must be >= 0";
    }

    std::unordered_set<std::string> topics;
    for (size_t i = 0; i < config.subscriptions.size(); ++i) {
        const auto& sub = config.subscriptions[i];
        if (sub.topic.empty()) {
            return "subscription topic must not be empty";
        }
        if (!topics.insert(sub.topic).second) {
            return "duplicate subscription topic: " + sub.topic;
        }
        if (sub.message_type.empty()) {
            return "subscription message_type must not be empty";
        }
        if (sub.queue_capacity == 0) {
            return "subscription queue_capacity must be > 0";
        }
        if (sub.mode == Ros2IngestMode::ScalarFields) {
            if (const auto err = validate_live_scalar_subscription(sub)) {
                return *err;
            }
        } else if (sub.mode == Ros2IngestMode::StandardProfile) {
            if (const auto err = validate_standard_profile_subscription(sub)) {
                return *err;
            }
        } else if (sub.mode == Ros2IngestMode::TypedProfile) {
            if (const auto err = validate_typed_profile_subscription(sub)) {
                return *err;
            }
        } else {
            return "unsupported ROS 2 ingest mode";
        }

        for (const auto& field : sub.fields) {
            if (field.field_path.empty()) {
                return "field_path must not be empty";
            }
            if (field.symbol_id == 0) {
                return "field symbol_id must be non-zero";
            }
            if (!std::isfinite(field.value_scale) || field.value_scale == 0.0) {
                return "field value_scale must be finite and non-zero";
            }
        }

        (void)i;
    }

    return std::nullopt;
}

std::optional<std::string>
Ros2Consumer::validate_live_scalar_subscription(const Ros2SubscriptionConfig& sub) {
    if (!is_supported_live_scalar_type(sub.message_type)) {
        return "unsupported live scalar message_type: " + sub.message_type;
    }
    if (sub.fields.size() != 1) {
        return "live scalar subscriptions require exactly one field mapping";
    }
    if (!sub.fields.empty() && sub.fields[0].field_path != "data") {
        return "live scalar subscriptions require field_path 'data'";
    }
    return std::nullopt;
}

std::optional<std::string>
Ros2Consumer::validate_standard_profile_subscription(const Ros2SubscriptionConfig& sub) {
    const auto profile = standard_profile_for_message_type(sub.message_type);
    if (profile == Ros2StandardProfile::None) {
        return "unsupported standard profile message_type: " + sub.message_type;
    }
    if (sub.fields.empty()) {
        return "standard profile subscriptions require at least one field mapping";
    }
    for (const auto& field : sub.fields) {
        if (!profile_field_supported(profile, field.field_path)) {
            return "unsupported field_path '" + field.field_path +
                   "' for message_type: " + sub.message_type;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
Ros2Consumer::validate_typed_profile_subscription(const Ros2SubscriptionConfig& sub) {
    const auto profile = standard_profile_for_message_type(sub.message_type);
    if (profile == Ros2StandardProfile::None) {
        return "unsupported typed profile message_type: " + sub.message_type;
    }
    if (sub.table_name.empty()) {
        return "typed profile subscriptions require table_name";
    }
    if (!sub.fields.empty()) {
        return "typed profile subscriptions do not use field mappings";
    }
    if (sub.typed_partition_symbol_id == 0) {
        return "typed profile partition symbol_id must be non-zero";
    }
    return std::nullopt;
}

std::optional<std::string>
Ros2Consumer::validate_bag_config(const Ros2BagConfig& bag_config) {
    if (bag_config.uri.empty()) {
        return "bag uri must not be empty";
    }
    if (!std::isfinite(bag_config.replay_speed) || bag_config.replay_speed <= 0.0) {
        return "bag replay_speed must be finite and > 0";
    }

    std::unordered_set<std::string> topics;
    for (const auto& topic : bag_config.topics) {
        if (topic.empty()) {
            return "bag topic filter entries must not be empty";
        }
        if (!topics.insert(topic).second) {
            return "duplicate bag topic filter: " + topic;
        }
    }

    return std::nullopt;
}

bool Ros2Consumer::is_supported_live_scalar_type(std::string_view message_type) noexcept {
    return message_type == "std_msgs/msg/Float64" ||
           message_type == "std_msgs/msg/Float32" ||
           message_type == "std_msgs/msg/Int64" ||
           message_type == "std_msgs/msg/Int32" ||
           message_type == "std_msgs/msg/UInt64" ||
           message_type == "std_msgs/msg/UInt32";
}

Ros2StandardProfile
Ros2Consumer::standard_profile_for_message_type(std::string_view message_type) noexcept {
    if (message_type == "sensor_msgs/msg/Imu") {
        return Ros2StandardProfile::Imu;
    }
    if (message_type == "sensor_msgs/msg/JointState") {
        return Ros2StandardProfile::JointState;
    }
    if (message_type == "nav_msgs/msg/Odometry") {
        return Ros2StandardProfile::Odometry;
    }
    if (message_type == "tf2_msgs/msg/TFMessage") {
        return Ros2StandardProfile::TfMessage;
    }
    if (message_type == "sensor_msgs/msg/LaserScan") {
        return Ros2StandardProfile::LaserScan;
    }
    return Ros2StandardProfile::None;
}

bool Ros2Consumer::is_supported_standard_profile_type(std::string_view message_type) noexcept {
    return standard_profile_for_message_type(message_type) != Ros2StandardProfile::None;
}

std::vector<zeptodb::storage::ColumnDef>
Ros2Consumer::typed_profile_schema(Ros2StandardProfile profile) {
    using zeptodb::storage::ColumnDef;
    using zeptodb::storage::ColumnType;

    auto schema = common_typed_schema();
    switch (profile) {
    case Ros2StandardProfile::Imu:
        append_schema(schema, {
            ColumnDef{"orientation_x", ColumnType::FLOAT64},
            ColumnDef{"orientation_y", ColumnType::FLOAT64},
            ColumnDef{"orientation_z", ColumnType::FLOAT64},
            ColumnDef{"orientation_w", ColumnType::FLOAT64},
            ColumnDef{"angular_velocity_x", ColumnType::FLOAT64},
            ColumnDef{"angular_velocity_y", ColumnType::FLOAT64},
            ColumnDef{"angular_velocity_z", ColumnType::FLOAT64},
            ColumnDef{"linear_acceleration_x", ColumnType::FLOAT64},
            ColumnDef{"linear_acceleration_y", ColumnType::FLOAT64},
            ColumnDef{"linear_acceleration_z", ColumnType::FLOAT64},
        });
        break;
    case Ros2StandardProfile::JointState:
        append_schema(schema, {
            ColumnDef{"joint_id", ColumnType::SYMBOL},
            ColumnDef{"position", ColumnType::FLOAT64},
            ColumnDef{"velocity", ColumnType::FLOAT64},
            ColumnDef{"effort", ColumnType::FLOAT64},
        });
        break;
    case Ros2StandardProfile::Odometry:
        append_schema(schema, {
            ColumnDef{"child_frame_id", ColumnType::SYMBOL},
            ColumnDef{"pose_position_x", ColumnType::FLOAT64},
            ColumnDef{"pose_position_y", ColumnType::FLOAT64},
            ColumnDef{"pose_position_z", ColumnType::FLOAT64},
            ColumnDef{"pose_orientation_x", ColumnType::FLOAT64},
            ColumnDef{"pose_orientation_y", ColumnType::FLOAT64},
            ColumnDef{"pose_orientation_z", ColumnType::FLOAT64},
            ColumnDef{"pose_orientation_w", ColumnType::FLOAT64},
            ColumnDef{"twist_linear_x", ColumnType::FLOAT64},
            ColumnDef{"twist_linear_y", ColumnType::FLOAT64},
            ColumnDef{"twist_linear_z", ColumnType::FLOAT64},
            ColumnDef{"twist_angular_x", ColumnType::FLOAT64},
            ColumnDef{"twist_angular_y", ColumnType::FLOAT64},
            ColumnDef{"twist_angular_z", ColumnType::FLOAT64},
        });
        break;
    case Ros2StandardProfile::TfMessage:
        append_schema(schema, {
            ColumnDef{"child_frame_id", ColumnType::SYMBOL},
            ColumnDef{"translation_x", ColumnType::FLOAT64},
            ColumnDef{"translation_y", ColumnType::FLOAT64},
            ColumnDef{"translation_z", ColumnType::FLOAT64},
            ColumnDef{"rotation_x", ColumnType::FLOAT64},
            ColumnDef{"rotation_y", ColumnType::FLOAT64},
            ColumnDef{"rotation_z", ColumnType::FLOAT64},
            ColumnDef{"rotation_w", ColumnType::FLOAT64},
        });
        break;
    case Ros2StandardProfile::LaserScan:
        append_schema(schema, {
            ColumnDef{"angle_min", ColumnType::FLOAT64},
            ColumnDef{"angle_max", ColumnType::FLOAT64},
            ColumnDef{"angle_increment", ColumnType::FLOAT64},
            ColumnDef{"time_increment", ColumnType::FLOAT64},
            ColumnDef{"scan_time", ColumnType::FLOAT64},
            ColumnDef{"range_min", ColumnType::FLOAT64},
            ColumnDef{"range_max", ColumnType::FLOAT64},
            ColumnDef{"ranges_count", ColumnType::INT64},
            ColumnDef{"ranges_min", ColumnType::FLOAT64},
            ColumnDef{"ranges_max", ColumnType::FLOAT64},
            ColumnDef{"ranges_mean", ColumnType::FLOAT64},
            ColumnDef{"intensities_count", ColumnType::INT64},
            ColumnDef{"intensities_min", ColumnType::FLOAT64},
            ColumnDef{"intensities_max", ColumnType::FLOAT64},
            ColumnDef{"intensities_mean", ColumnType::FLOAT64},
        });
        break;
    case Ros2StandardProfile::None:
        schema.clear();
        break;
    }
    return schema;
}

std::optional<uint64_t> Ros2Consumer::time_to_ns(Ros2Time time) noexcept {
    if (time.sec < 0 || time.nanosec >= kNanosecondsPerSecond) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(time.sec) * kNanosecondsPerSecond + time.nanosec;
}

std::optional<int64_t>
Ros2Consumer::scale_numeric_value(long double value, long double scale) noexcept {
    if (!std::isfinite(value) || !std::isfinite(scale) || scale == 0.0L) {
        return std::nullopt;
    }
    const long double scaled = value * scale;
    if (!std::isfinite(scaled) ||
        scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        scaled < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::nullopt;
    }
    return static_cast<int64_t>(scaled);
}

std::optional<zeptodb::ingestion::TickMessage>
Ros2Consumer::scalar_sample_to_tick(const Ros2ScalarSample& sample) {
    if (sample.symbol_id == 0) {
        return std::nullopt;
    }

    const uint64_t ts = sample.source_ts_ns != 0 ? sample.source_ts_ns : sample.recv_ts_ns;
    zeptodb::ingestion::TickMessage msg{};
    msg.symbol_id = sample.symbol_id;
    msg.price     = static_cast<Price>(sample.value);
    msg.volume    = static_cast<Volume>(sample.quality);
    msg.recv_ts   = static_cast<Timestamp>(ts);
    msg.msg_type  = 0;
    return msg;
}

std::vector<Ros2ScalarSample>
Ros2Consumer::imu_sample_to_scalars(const Ros2ImuSample& sample,
                                    const Ros2SubscriptionConfig& sub) {
    std::vector<Ros2ScalarSample> out;
    out.reserve(sub.fields.size());
    for (const auto& field : sub.fields) {
        const auto value = imu_field_value(sample, field.field_path);
        if (!value) {
            continue;
        }
        append_scalar_if_valid(out, sample.topic, field, *value, sample.header.stamp_ns,
                               sample.recv_ts_ns, sample.quality);
    }
    return out;
}

std::vector<Ros2ScalarSample>
Ros2Consumer::joint_state_sample_to_scalars(const Ros2JointStateSample& sample,
                                            const Ros2SubscriptionConfig& sub) {
    const size_t joint_count = std::max(
        sample.names.size(),
        std::max(sample.positions.size(), std::max(sample.velocities.size(), sample.efforts.size())));

    std::vector<Ros2ScalarSample> out;
    out.reserve(joint_count * sub.fields.size());
    for (size_t i = 0; i < joint_count; ++i) {
        for (const auto& field : sub.fields) {
            const std::vector<long double>* values = nullptr;
            if (field.field_path == "position") {
                values = &sample.positions;
            } else if (field.field_path == "velocity") {
                values = &sample.velocities;
            } else if (field.field_path == "effort") {
                values = &sample.efforts;
            }
            if (!values || i >= values->size()) {
                continue;
            }
            append_scalar_if_valid(out, sample.topic, field, (*values)[i],
                                   sample.header.stamp_ns, sample.recv_ts_ns,
                                   sample.quality, i);
        }
    }
    return out;
}

std::vector<Ros2ScalarSample>
Ros2Consumer::odometry_sample_to_scalars(const Ros2OdometrySample& sample,
                                         const Ros2SubscriptionConfig& sub) {
    std::vector<Ros2ScalarSample> out;
    out.reserve(sub.fields.size());
    for (const auto& field : sub.fields) {
        const auto value = odometry_field_value(sample, field.field_path);
        if (!value) {
            continue;
        }
        append_scalar_if_valid(out, sample.topic, field, *value, sample.header.stamp_ns,
                               sample.recv_ts_ns, sample.quality);
    }
    return out;
}

std::vector<Ros2ScalarSample>
Ros2Consumer::tf_message_sample_to_scalars(const Ros2TfMessageSample& sample,
                                           const Ros2SubscriptionConfig& sub) {
    std::vector<Ros2ScalarSample> out;
    out.reserve(sample.transforms.size() * sub.fields.size());
    for (size_t i = 0; i < sample.transforms.size(); ++i) {
        const auto& transform = sample.transforms[i];
        for (const auto& field : sub.fields) {
            const auto value = tf_field_value(transform, field.field_path);
            if (!value) {
                continue;
            }
            const uint64_t source_ts =
                transform.header.stamp_ns != 0 ? transform.header.stamp_ns : 0;
            append_scalar_if_valid(out, sample.topic, field, *value, source_ts,
                                   sample.recv_ts_ns, sample.quality, i);
        }
    }
    return out;
}

std::vector<Ros2ScalarSample>
Ros2Consumer::laser_scan_sample_to_scalars(const Ros2LaserScanSample& sample,
                                           const Ros2SubscriptionConfig& sub) {
    const auto ranges = summarize_finite(sample.ranges);
    const auto intensities = summarize_finite(sample.intensities);

    std::vector<Ros2ScalarSample> out;
    out.reserve(sub.fields.size());
    for (const auto& field : sub.fields) {
        const auto value = laser_scan_field_value(sample, ranges, intensities, field.field_path);
        if (!value) {
            continue;
        }
        append_scalar_if_valid(out, sample.topic, field, *value, sample.header.stamp_ns,
                               sample.recv_ts_ns, sample.quality);
    }
    return out;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool Ros2Consumer::start() {
    if (const auto err = validate_config(config_)) {
        ZEPTO_ERROR("Ros2Consumer: invalid config: {}", *err);
        return false;
    }
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("ROS 2 consumer requires Enterprise license (IOT_CONNECTORS) — not starting");
        return false;
    }

#if defined(ZEPTO_ROS2_AVAILABLE) && !defined(ZEPTO_ROS2_STANDARD_AVAILABLE)
    for (const auto& sub : config_.subscriptions) {
        if (sub.mode == Ros2IngestMode::StandardProfile ||
            sub.mode == Ros2IngestMode::TypedProfile) {
            ZEPTO_WARN("Ros2Consumer: standard profile '{}' requires sensor_msgs/nav_msgs/tf2_msgs "
                       "(reconfigure with ROS 2 standard message packages installed)",
                       sub.message_type);
            return false;
        }
    }
#endif

#ifdef ZEPTO_ROS2_AVAILABLE
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }

    try {
        auto runtime = std::make_unique<Ros2Runtime>();
        runtime->context = std::make_shared<rclcpp::Context>();
        runtime->context->init(0, nullptr);

        rclcpp::NodeOptions node_options;
        node_options.context(runtime->context);
        runtime->node = std::make_shared<rclcpp::Node>(config_.node_name, node_options);

        rclcpp::ExecutorOptions exec_options;
        exec_options.context = runtime->context;
        runtime->executor =
            std::make_unique<rclcpp::executors::SingleThreadedExecutor>(exec_options);
        runtime->executor->add_node(runtime->node);

        for (const auto& sub : config_.subscriptions) {
            rclcpp::QoS qos(rclcpp::KeepLast(sub.queue_capacity));
            if (sub.drop_on_full) {
                qos.best_effort();
            } else {
                qos.reliable();
            }

            const auto topic = sub.topic;
            if (sub.mode == Ros2IngestMode::ScalarFields) {
                const auto& field = sub.fields.front();
                const auto symbol_id = field.symbol_id;
                const auto scale = static_cast<long double>(field.value_scale);

                if (sub.message_type == "std_msgs/msg/Float64") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::Float64>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::Float64::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                } else if (sub.message_type == "std_msgs/msg/Float32") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::Float32>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::Float32::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                } else if (sub.message_type == "std_msgs/msg/Int64") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::Int64>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::Int64::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                } else if (sub.message_type == "std_msgs/msg/Int32") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::Int32>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::Int32::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                } else if (sub.message_type == "std_msgs/msg/UInt64") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::UInt64>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::UInt64::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                } else if (sub.message_type == "std_msgs/msg/UInt32") {
                    runtime->subscriptions.push_back(
                        runtime->node->create_subscription<std_msgs::msg::UInt32>(
                            topic, qos, [this, topic, symbol_id, scale]
                            (const std_msgs::msg::UInt32::SharedPtr msg) {
                                handle_live_scalar(topic, symbol_id, msg->data, scale);
                            }));
                }
                continue;
            }

#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
            const auto profile = standard_profile_for_message_type(sub.message_type);
            const bool typed = sub.mode == Ros2IngestMode::TypedProfile;
            if (profile == Ros2StandardProfile::Imu) {
                runtime->subscriptions.push_back(
                    runtime->node->create_subscription<sensor_msgs::msg::Imu>(
                        topic, qos, [this, topic, sub, typed]
                        (const sensor_msgs::msg::Imu::SharedPtr msg) {
                            auto sample = imu_from_ros(*msg, topic);
                            if (typed) {
                                if (!pipeline_) {
                                    record_decode_error();
                                    return;
                                }
                                handle_typed_rows(
                                    topic,
                                    imu_typed_rows(sample, sub, pipeline_->symbol_dict(),
                                                   config_.robot_id, config_.session_id));
                                return;
                            }
                            handle_standard_scalars(imu_sample_to_scalars(sample, sub));
                        }));
            } else if (profile == Ros2StandardProfile::JointState) {
                runtime->subscriptions.push_back(
                    runtime->node->create_subscription<sensor_msgs::msg::JointState>(
                        topic, qos, [this, topic, sub, typed]
                        (const sensor_msgs::msg::JointState::SharedPtr msg) {
                            auto sample = joint_state_from_ros(*msg, topic);
                            if (typed) {
                                if (!pipeline_) {
                                    record_decode_error();
                                    return;
                                }
                                handle_typed_rows(
                                    topic,
                                    joint_state_typed_rows(sample, sub, pipeline_->symbol_dict(),
                                                           config_.robot_id, config_.session_id));
                                return;
                            }
                            handle_standard_scalars(joint_state_sample_to_scalars(sample, sub));
                        }));
            } else if (profile == Ros2StandardProfile::Odometry) {
                runtime->subscriptions.push_back(
                    runtime->node->create_subscription<nav_msgs::msg::Odometry>(
                        topic, qos, [this, topic, sub, typed]
                        (const nav_msgs::msg::Odometry::SharedPtr msg) {
                            auto sample = odometry_from_ros(*msg, topic);
                            if (typed) {
                                if (!pipeline_) {
                                    record_decode_error();
                                    return;
                                }
                                handle_typed_rows(
                                    topic,
                                    odometry_typed_rows(sample, sub, pipeline_->symbol_dict(),
                                                        config_.robot_id, config_.session_id));
                                return;
                            }
                            handle_standard_scalars(odometry_sample_to_scalars(sample, sub));
                        }));
            } else if (profile == Ros2StandardProfile::TfMessage) {
                runtime->subscriptions.push_back(
                    runtime->node->create_subscription<tf2_msgs::msg::TFMessage>(
                        topic, qos, [this, topic, sub, typed]
                        (const tf2_msgs::msg::TFMessage::SharedPtr msg) {
                            auto sample = tf_message_from_ros(*msg, topic);
                            if (typed) {
                                if (!pipeline_) {
                                    record_decode_error();
                                    return;
                                }
                                handle_typed_rows(
                                    topic,
                                    tf_message_typed_rows(sample, sub, pipeline_->symbol_dict(),
                                                          config_.robot_id, config_.session_id));
                                return;
                            }
                            handle_standard_scalars(tf_message_sample_to_scalars(sample, sub));
                        }));
            } else if (profile == Ros2StandardProfile::LaserScan) {
                runtime->subscriptions.push_back(
                    runtime->node->create_subscription<sensor_msgs::msg::LaserScan>(
                        topic, qos, [this, topic, sub, typed]
                        (const sensor_msgs::msg::LaserScan::SharedPtr msg) {
                            auto sample = laser_scan_from_ros(*msg, topic);
                            if (typed) {
                                if (!pipeline_) {
                                    record_decode_error();
                                    return;
                                }
                                handle_typed_rows(
                                    topic,
                                    laser_scan_typed_rows(sample, sub, pipeline_->symbol_dict(),
                                                          config_.robot_id, config_.session_id));
                                return;
                            }
                            handle_standard_scalars(laser_scan_sample_to_scalars(sample, sub));
                        }));
            }
#endif
        }

        runtime->spin_thread = std::thread([exec = runtime->executor.get()] {
            exec->spin();
        });

        runtime_handle_ = runtime.release();
        running_.store(true, std::memory_order_release);
        ZEPTO_INFO("Ros2Consumer: subscribed to {} ROS 2 topic(s)",
                   config_.subscriptions.size());
        return true;
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_release);
        ZEPTO_ERROR("Ros2Consumer: failed to start ROS 2 runtime: {}", e.what());
        return false;
    }
#else
    ZEPTO_WARN("Ros2Consumer: ROS 2 support not compiled in "
               "(rebuild with ZEPTO_USE_ROS2=ON and ROS 2 development packages installed)");
    return false;
#endif
}

void Ros2Consumer::stop() {
    (void)runtime_handle_;
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

#ifdef ZEPTO_ROS2_AVAILABLE
    if (runtime_handle_) {
        auto* runtime = static_cast<Ros2Runtime*>(runtime_handle_);
        if (runtime->executor) {
            runtime->executor->cancel();
        }
        if (runtime->spin_thread.joinable()) {
            runtime->spin_thread.join();
        }
        if (runtime->executor && runtime->node) {
            runtime->executor->remove_node(runtime->node);
        }
        if (runtime->context && runtime->context->is_valid()) {
            runtime->context->shutdown("Ros2Consumer stopped");
        }
        delete runtime;
        runtime_handle_ = nullptr;
    }
#endif
}

// ============================================================================
// rosbag2 import / replay
// ============================================================================

Ros2BagStats Ros2Consumer::import_bag(const Ros2BagConfig& bag_config) {
    return consume_bag(bag_config, false);
}

Ros2BagStats Ros2Consumer::replay_bag(const Ros2BagConfig& bag_config) {
    return consume_bag(bag_config, true);
}

#ifdef ZEPTO_ROS2_BAG_AVAILABLE
Ros2BagStats Ros2Consumer::consume_bag(const Ros2BagConfig& bag_config, bool replay) {
    Ros2BagStats bag_stats;

    if (const auto err = validate_config(config_)) {
        bag_stats.error = "invalid ROS 2 config: " + *err;
        ZEPTO_ERROR("Ros2Consumer: {}", bag_stats.error);
        return bag_stats;
    }
    if (const auto err = validate_bag_config(bag_config)) {
        bag_stats.error = *err;
        ZEPTO_ERROR("Ros2Consumer: invalid rosbag2 config: {}", bag_stats.error);
        return bag_stats;
    }
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        bag_stats.error = "ROS 2 bag import/replay requires Enterprise license (IOT_CONNECTORS)";
        ZEPTO_WARN("Ros2Consumer: {}", bag_stats.error);
        return bag_stats;
    }
#if !defined(ZEPTO_ROS2_STANDARD_AVAILABLE)
    for (const auto& sub : config_.subscriptions) {
        if (sub.mode == Ros2IngestMode::StandardProfile ||
            sub.mode == Ros2IngestMode::TypedProfile) {
            bag_stats.error =
                "ROS 2 standard profile bag import/replay requires sensor_msgs/nav_msgs/tf2_msgs";
            ZEPTO_WARN("Ros2Consumer: {}", bag_stats.error);
            return bag_stats;
        }
    }
#endif

    try {
        rosbag2_cpp::Reader reader;
        reader.open(bag_config.uri);
        reader.set_read_order(
            rosbag2_storage::ReadOrder(rosbag2_storage::ReadOrder::ReceivedTimestamp, false));

        std::unordered_map<std::string, std::string> bag_topic_types;
        for (const auto& topic : reader.get_all_topics_and_types()) {
            bag_topic_types.emplace(topic.name, topic.type);
        }

        rosbag2_storage::StorageFilter filter;
        if (!bag_config.topics.empty()) {
            filter.topics = bag_config.topics;
        } else {
            filter.topics.reserve(config_.subscriptions.size());
            for (const auto& sub : config_.subscriptions) {
                filter.topics.push_back(sub.topic);
            }
        }
        reader.set_filter(filter);

        uint64_t previous_replay_ts = 0;
        while (reader.has_next()) {
            if (bag_config.max_messages != 0 &&
                bag_stats.messages_read >= bag_config.max_messages) {
                break;
            }

            const auto bag_msg = reader.read_next();
            bag_stats.messages_read++;
            if (!bag_msg) {
                bag_stats.decode_errors++;
                record_decode_error();
                continue;
            }

            const auto* sub = subscription_for_topic(bag_msg->topic_name);
            if (!sub) {
                if (bag_config.fail_on_unknown_topic) {
                    bag_stats.error = "unknown bag topic: " + bag_msg->topic_name;
                    ZEPTO_ERROR("Ros2Consumer: {}", bag_stats.error);
                    return bag_stats;
                }
                bag_stats.messages_skipped++;
                continue;
            }

            const auto type_it = bag_topic_types.find(bag_msg->topic_name);
            if (type_it == bag_topic_types.end() || type_it->second != sub->message_type) {
                bag_stats.decode_errors++;
                record_decode_error();
                continue;
            }

            const uint64_t recv_ts = positive_timestamp_ns(bag_msg->recv_timestamp);
            uint64_t source_ts = positive_timestamp_ns(bag_msg->send_timestamp);
            if (source_ts == 0) {
                source_ts = recv_ts;
            }

            std::vector<Ros2ScalarSample> samples;
            std::vector<zeptodb::core::TypedRowMessage> typed_rows;
            if (sub->mode == Ros2IngestMode::ScalarFields) {
                const auto decoded = decode_rosbag_scalar_value(sub->message_type, *bag_msg);
                if (!decoded) {
                    bag_stats.decode_errors++;
                    record_decode_error();
                    continue;
                }

                const auto& field = sub->fields.front();
                const auto scaled = scale_numeric_value(
                    *decoded, static_cast<long double>(field.value_scale));
                if (!scaled) {
                    bag_stats.decode_errors++;
                    record_decode_error();
                    continue;
                }

                Ros2ScalarSample sample;
                sample.topic = bag_msg->topic_name;
                sample.symbol_id = field.symbol_id;
                sample.value = *scaled;
                sample.source_ts_ns = source_ts;
                sample.recv_ts_ns = recv_ts;
                sample.quality = 1;
                samples.push_back(sample);
            } else if (sub->mode == Ros2IngestMode::StandardProfile) {
#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
                samples = decode_rosbag_standard_profile_scalars(*sub, *bag_msg, source_ts, recv_ts);
#else
                (void)source_ts;
                (void)recv_ts;
#endif
                if (samples.empty()) {
                    bag_stats.decode_errors++;
                    record_decode_error();
                    continue;
                }
            } else {
#ifdef ZEPTO_ROS2_STANDARD_AVAILABLE
                if (!pipeline_) {
                    bag_stats.decode_errors++;
                    record_decode_error();
                    continue;
                }
                typed_rows = decode_rosbag_typed_profile_rows(
                    *sub, *bag_msg, source_ts, recv_ts, pipeline_->symbol_dict(),
                    config_.robot_id, config_.session_id);
#else
                (void)source_ts;
                (void)recv_ts;
#endif
                if (typed_rows.empty()) {
                    bag_stats.decode_errors++;
                    record_decode_error();
                    continue;
                }
            }

            if (source_ts != 0) {
                if (bag_stats.first_source_ts_ns == 0) {
                    bag_stats.first_source_ts_ns = source_ts;
                }
                bag_stats.last_source_ts_ns = source_ts;
            }

            if (replay && previous_replay_ts != 0 && source_ts > previous_replay_ts) {
                const long double sleep_ns =
                    static_cast<long double>(source_ts - previous_replay_ts) /
                    static_cast<long double>(bag_config.replay_speed);
                if (sleep_ns > 0.0L &&
                    sleep_ns < static_cast<long double>(std::numeric_limits<int64_t>::max())) {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds(static_cast<int64_t>(sleep_ns)));
                }
            }
            if (source_ts != 0) {
                previous_replay_ts = source_ts;
            }

            bag_stats.messages_consumed++;
            if (sub->mode == Ros2IngestMode::TypedProfile) {
                {
                    std::lock_guard<std::mutex> lk(stats_mu_);
                    stats_.messages_consumed++;
                }
                for (auto& row : typed_rows) {
                    if (on_typed_row(sub->topic, std::move(row))) {
                        bag_stats.rows_ingested++;
                    }
                }
            } else {
                for (const auto& sample : samples) {
                    if (on_scalar_sample(sample)) {
                        bag_stats.rows_ingested++;
                    }
                }
            }
        }

        bag_stats.completed = true;
        return bag_stats;
    } catch (const std::exception& e) {
        bag_stats.error = e.what();
        ZEPTO_ERROR("Ros2Consumer: rosbag2 import/replay failed: {}", bag_stats.error);
        return bag_stats;
    }
}
#else
Ros2BagStats Ros2Consumer::consume_bag(const Ros2BagConfig& bag_config, bool replay) {
    (void)bag_config;
    (void)replay;
    Ros2BagStats bag_stats;
    bag_stats.error =
        "rosbag2 support not compiled in (rebuild with ZEPTO_USE_ROS2=ON and rosbag2_cpp)";
    ZEPTO_WARN("Ros2Consumer: {}", bag_stats.error);
    return bag_stats;
}
#endif

// ============================================================================
// Ingestion
// ============================================================================

bool Ros2Consumer::has_topic(const std::string& topic) const {
    return topic_bindings_.find(topic) != topic_bindings_.end();
}

const Ros2SubscriptionConfig*
Ros2Consumer::subscription_for_topic(const std::string& topic) const {
    for (const auto& sub : config_.subscriptions) {
        if (sub.topic == topic) {
            return &sub;
        }
    }
    return nullptr;
}

Ros2Consumer::TopicBinding Ros2Consumer::binding_for_topic(const std::string& topic) const {
    auto it = topic_bindings_.find(topic);
    if (it == topic_bindings_.end()) {
        return {};
    }
    return it->second;
}

void Ros2Consumer::record_decode_error() {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.decode_errors++;
}

void Ros2Consumer::handle_live_scalar(const std::string& topic,
                                      SymbolId symbol_id,
                                      long double value,
                                      long double scale) {
    auto scaled = scale_numeric_value(value, scale);
    if (!scaled) {
        record_decode_error();
        return;
    }

    const auto recv_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    Ros2ScalarSample sample;
    sample.topic = topic;
    sample.symbol_id = symbol_id;
    sample.value = *scaled;
    sample.recv_ts_ns = recv_ts;
    sample.quality = 1;
    on_scalar_sample(sample);
}

void Ros2Consumer::handle_standard_scalars(const std::vector<Ros2ScalarSample>& samples) {
    if (samples.empty()) {
        record_decode_error();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.messages_consumed++;
    }
    for (const auto& sample : samples) {
        (void)on_scalar_sample_impl(sample, false);
    }
}

void Ros2Consumer::handle_typed_rows(const std::string& topic,
                                     std::vector<zeptodb::core::TypedRowMessage> rows) {
    if (rows.empty()) {
        record_decode_error();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.messages_consumed++;
    }
    for (auto& row : rows) {
        (void)on_typed_row(topic, std::move(row));
    }
}

bool Ros2Consumer::on_scalar_sample(const Ros2ScalarSample& sample) {
    return on_scalar_sample_impl(sample, true);
}

bool Ros2Consumer::on_scalar_sample_impl(const Ros2ScalarSample& sample, bool count_message) {
    if (!has_topic(sample.topic)) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }

    auto tick = scalar_sample_to_tick(sample);
    if (!tick) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }

    const TopicBinding binding = binding_for_topic(sample.topic);
    if (!binding.table_name.empty() && binding.table_id == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (count_message) {
            stats_.messages_consumed++;
        }
        stats_.bytes_consumed += sizeof(int64_t);
        stats_.messages_dropped++;
        stats_.ingest_failures++;
        return false;
    }
    tick->table_id = binding.table_id;

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (count_message) {
            stats_.messages_consumed++;
        }
        stats_.bytes_consumed += sizeof(int64_t);
        if (sample.source_ts_ns != 0 && sample.recv_ts_ns != 0) {
            stats_.last_source_lag_ns =
                static_cast<int64_t>(sample.recv_ts_ns) -
                static_cast<int64_t>(sample.source_ts_ns);
        }
    }

    return ingest_decoded(*tick);
}

bool Ros2Consumer::on_typed_row(const std::string& topic,
                                zeptodb::core::TypedRowMessage row) {
    if (!has_topic(topic)) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }

    const TopicBinding binding = binding_for_topic(topic);
    if (binding.table_name.empty() || binding.table_id == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.bytes_consumed += typed_row_payload_bytes(row);
        stats_.messages_dropped++;
        stats_.ingest_failures++;
        return false;
    }

    row.table_id = binding.table_id;
    const size_t payload_bytes = typed_row_payload_bytes(row);
    const uint64_t recv_ts = typed_row_recv_ts(row);
    const uint64_t source_ts = row.timestamp > 0 ? static_cast<uint64_t>(row.timestamp) : 0;

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.bytes_consumed += payload_bytes;
        if (source_ts != 0 && recv_ts != 0) {
            stats_.last_source_lag_ns =
                static_cast<int64_t>(recv_ts) - static_cast<int64_t>(source_ts);
        }
    }

    if (router_) {
        const zeptodb::cluster::NodeId target = router_->route(row.table_id, row.symbol_id);
        if (target != local_id_) {
            auto it = remotes_.find(target);
            if (it == remotes_.end()) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.messages_dropped++;
                stats_.ingest_failures++;
                return false;
            }
            const bool ok = try_with_backpressure(
                [&] { return it->second->ingest_typed_row(row); },
                config_.backpressure_retries,
                config_.backpressure_sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) {
                stats_.route_remote++;
                stats_.rows_ingested++;
            } else {
                stats_.messages_dropped++;
                stats_.ingest_failures++;
            }
            return ok;
        }
    }

    if (!pipeline_) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.ingest_failures++;
        return false;
    }

    const bool ok = pipeline_->ingest_typed_row(std::move(row));
    std::lock_guard<std::mutex> lk(stats_mu_);
    if (ok) {
        stats_.route_local++;
        stats_.rows_ingested++;
    } else {
        stats_.messages_dropped++;
        stats_.ingest_failures++;
    }
    return ok;
}

bool Ros2Consumer::ingest_decoded(zeptodb::ingestion::TickMessage msg) {
    const int retries  = config_.backpressure_retries;
    const int sleep_us = config_.backpressure_sleep_us;

    if (router_) {
        const zeptodb::cluster::NodeId target = router_->route(msg.table_id, msg.symbol_id);
        if (target == local_id_) {
            if (!pipeline_) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.ingest_failures++;
                return false;
            }
            const bool ok = try_with_backpressure(
                [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) {
                stats_.route_local++;
                stats_.rows_ingested++;
            } else {
                stats_.messages_dropped++;
                stats_.ingest_failures++;
            }
            return ok;
        }

        auto it = remotes_.find(target);
        if (it == remotes_.end()) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.ingest_failures++;
            return false;
        }
        const bool ok = try_with_backpressure(
            [&] { return it->second->ingest_tick(msg); }, retries, sleep_us);
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (ok) {
            stats_.route_remote++;
            stats_.rows_ingested++;
        } else {
            stats_.messages_dropped++;
            stats_.ingest_failures++;
        }
        return ok;
    }

    if (pipeline_) {
        const bool ok = try_with_backpressure(
            [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (ok) {
            stats_.route_local++;
            stats_.rows_ingested++;
        } else {
            stats_.messages_dropped++;
            stats_.ingest_failures++;
        }
        return ok;
    }

    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.ingest_failures++;
    return false;
}

// ============================================================================
// Stats and metrics
// ============================================================================

Ros2Stats Ros2Consumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

std::string Ros2Consumer::format_prometheus(const std::string& bridge_name,
                                            const Ros2Stats& stats) {
    std::ostringstream os;
    const auto& n = bridge_name;

    os << "# HELP zepto_ros2_messages_consumed_total Messages received from ROS 2 subscriptions\n";
    os << "# TYPE zepto_ros2_messages_consumed_total counter\n";
    os << "zepto_ros2_messages_consumed_total{bridge=\"" << n << "\"} "
       << stats.messages_consumed << "\n\n";

    os << "# HELP zepto_ros2_rows_ingested_total Rows written to ZeptoDB\n";
    os << "# TYPE zepto_ros2_rows_ingested_total counter\n";
    os << "zepto_ros2_rows_ingested_total{bridge=\"" << n << "\"} "
       << stats.rows_ingested << "\n\n";

    os << "# HELP zepto_ros2_bytes_consumed_total Scalar payload bytes received from ROS 2\n";
    os << "# TYPE zepto_ros2_bytes_consumed_total counter\n";
    os << "zepto_ros2_bytes_consumed_total{bridge=\"" << n << "\"} "
       << stats.bytes_consumed << "\n\n";

    os << "# HELP zepto_ros2_decode_errors_total Message mapping failures\n";
    os << "# TYPE zepto_ros2_decode_errors_total counter\n";
    os << "zepto_ros2_decode_errors_total{bridge=\"" << n << "\"} "
       << stats.decode_errors << "\n\n";

    os << "# HELP zepto_ros2_messages_dropped_total Drops from queue or backpressure policy\n";
    os << "# TYPE zepto_ros2_messages_dropped_total counter\n";
    os << "zepto_ros2_messages_dropped_total{bridge=\"" << n << "\"} "
       << stats.messages_dropped << "\n\n";

    os << "# HELP zepto_ros2_route_local_total Rows routed to the local pipeline\n";
    os << "# TYPE zepto_ros2_route_local_total counter\n";
    os << "zepto_ros2_route_local_total{bridge=\"" << n << "\"} "
       << stats.route_local << "\n\n";

    os << "# HELP zepto_ros2_route_remote_total Rows forwarded through cluster routing\n";
    os << "# TYPE zepto_ros2_route_remote_total counter\n";
    os << "zepto_ros2_route_remote_total{bridge=\"" << n << "\"} "
       << stats.route_remote << "\n\n";

    os << "# HELP zepto_ros2_ingest_failures_total Pipeline or RPC ingest failures\n";
    os << "# TYPE zepto_ros2_ingest_failures_total counter\n";
    os << "zepto_ros2_ingest_failures_total{bridge=\"" << n << "\"} "
       << stats.ingest_failures << "\n\n";

    os << "# HELP zepto_ros2_source_lag_ns Last observed recv_ts_ns - source_ts_ns\n";
    os << "# TYPE zepto_ros2_source_lag_ns gauge\n";
    os << "zepto_ros2_source_lag_ns{bridge=\"" << n << "\"} "
       << stats.last_source_lag_ns << "\n";

    return os.str();
}

} // namespace zeptodb::feeds

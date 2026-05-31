// ============================================================================
// ZeptoDB: ROS 2 Consumer — bridge implementation
// ============================================================================

#include "zeptodb/feeds/ros2_consumer.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

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
        (void)topic;
        binding.table_id = 0;
        if (pipeline_ && !binding.table_name.empty()) {
            const uint16_t tid = pipeline_->schema_registry().get_table_id(binding.table_name);
            if (tid == 0) {
                ZEPTO_ERROR("Ros2Consumer: unknown table '{}' — topic '{}' rows will be dropped",
                            binding.table_name, topic);
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
        if (sub.mode != Ros2IngestMode::ScalarFields) {
            return "only ScalarFields mode is supported in the ROS 2 bridge";
        }
        if (sub.fields.empty()) {
            return "subscription fields must not be empty";
        }
        if (const auto err = validate_live_scalar_subscription(sub)) {
            return *err;
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
            const auto& field = sub.fields.front();
            rclcpp::QoS qos(rclcpp::KeepLast(sub.queue_capacity));
            if (sub.drop_on_full) {
                qos.best_effort();
            } else {
                qos.reliable();
            }

            const auto topic = sub.topic;
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
        }

        runtime->spin_thread = std::thread([exec = runtime->executor.get()] {
            exec->spin();
        });

        runtime_handle_ = runtime.release();
        running_.store(true, std::memory_order_release);
        ZEPTO_INFO("Ros2Consumer: subscribed to {} ROS 2 scalar topic(s)",
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

            const uint64_t recv_ts = positive_timestamp_ns(bag_msg->recv_timestamp);
            uint64_t source_ts = positive_timestamp_ns(bag_msg->send_timestamp);
            if (source_ts == 0) {
                source_ts = recv_ts;
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

            Ros2ScalarSample sample;
            sample.topic = bag_msg->topic_name;
            sample.symbol_id = field.symbol_id;
            sample.value = *scaled;
            sample.source_ts_ns = source_ts;
            sample.recv_ts_ns = recv_ts;
            sample.quality = 1;

            bag_stats.messages_consumed++;
            if (on_scalar_sample(sample)) {
                bag_stats.rows_ingested++;
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

bool Ros2Consumer::on_scalar_sample(const Ros2ScalarSample& sample) {
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
        stats_.messages_consumed++;
        stats_.bytes_consumed += sizeof(int64_t);
        stats_.messages_dropped++;
        stats_.ingest_failures++;
        return false;
    }
    tick->table_id = binding.table_id;

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.messages_consumed++;
        stats_.bytes_consumed += sizeof(int64_t);
        if (sample.source_ts_ns != 0 && sample.recv_ts_ns != 0) {
            stats_.last_source_lag_ns =
                static_cast<int64_t>(sample.recv_ts_ns) -
                static_cast<int64_t>(sample.source_ts_ns);
        }
    }

    return ingest_decoded(*tick);
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

// ============================================================================
// ZeptoDB: MQTT Consumer — implementation
// ============================================================================

#include "zeptodb/feeds/mqtt_consumer.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <thread>

#ifdef ZEPTO_MQTT_AVAILABLE
#include <mqtt/async_client.h>
#endif

namespace zeptodb::feeds {

// ============================================================================
// Constructor / Destructor
// ============================================================================

MqttConsumer::MqttConsumer(MqttConfig config)
    : config_(std::move(config)) {}

MqttConsumer::~MqttConsumer() {
    stop();
}

// ============================================================================
// Routing setup
// ============================================================================

void MqttConsumer::set_pipeline(zeptodb::core::ZeptoPipeline* pipeline) {
    pipeline_ = pipeline;
}

void MqttConsumer::set_routing(
    zeptodb::cluster::NodeId local_id,
    std::shared_ptr<zeptodb::cluster::PartitionRouter> router,
    std::unordered_map<zeptodb::cluster::NodeId,
        std::shared_ptr<zeptodb::cluster::TcpRpcClient>> remotes)
{
    local_id_ = local_id;
    router_   = std::move(router);
    remotes_  = std::move(remotes);
}

// ============================================================================
// Dispatch (routing) — mirrors KafkaConsumer::ingest_decoded
// ============================================================================
namespace {
template <typename Fn>
bool try_with_backpressure(Fn ingest_fn, int retries, int sleep_us) {
    for (int i = 0; i <= retries; ++i) {
        if (ingest_fn()) return true;
        if (i < retries)
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
    return false;
}
} // namespace

bool MqttConsumer::ingest_decoded(zeptodb::ingestion::TickMessage msg) {
    const int retries  = config_.backpressure_retries;
    const int sleep_us = config_.backpressure_sleep_us;

    if (router_) {
        zeptodb::cluster::NodeId target = router_->route(msg.symbol_id);
        if (target == local_id_) {
            if (!pipeline_) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.ingest_failures++;
                return false;
            }
            bool ok = try_with_backpressure(
                [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) stats_.route_local++;
            else    stats_.ingest_failures++;
            return ok;
        } else {
            auto it = remotes_.find(target);
            if (it == remotes_.end()) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.ingest_failures++;
                return false;
            }
            bool ok = try_with_backpressure(
                [&] { return it->second->ingest_tick(msg); }, retries, sleep_us);
            std::lock_guard<std::mutex> lk(stats_mu_);
            if (ok) stats_.route_remote++;
            else    stats_.ingest_failures++;
            return ok;
        }
    } else if (pipeline_) {
        bool ok = try_with_backpressure(
            [&] { return pipeline_->ingest_tick(msg); }, retries, sleep_us);
        std::lock_guard<std::mutex> lk(stats_mu_);
        if (ok) stats_.route_local++;
        else    stats_.ingest_failures++;
        return ok;
    }
    return false;
}

// ============================================================================
// on_message: decode + dispatch
// ============================================================================

bool MqttConsumer::on_message(const char* data, size_t len) {
    std::optional<zeptodb::ingestion::TickMessage> tick;

    switch (config_.format) {
        case MessageFormat::JSON:
            tick = decode_json(data, len);
            break;
        case MessageFormat::BINARY:
            tick = decode_binary(data, len);
            break;
        case MessageFormat::JSON_HUMAN:
            tick = decode_json_human(data, len,
                                     config_.symbol_map,
                                     config_.price_scale);
            break;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.bytes_consumed += len;
        if (!tick) {
            stats_.decode_errors++;
            return false;
        }
        stats_.messages_consumed++;
    }

    return ingest_decoded(*tick);
}

// ============================================================================
// Lifecycle: start / stop
// ============================================================================

bool MqttConsumer::start() {
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("MQTT consumer requires Enterprise license (IOT_CONNECTORS) — not starting");
        return false;
    }
    if (config_.topic.empty()) {
        ZEPTO_ERROR("MqttConsumer: topic must not be empty");
        return false;
    }
    if (!is_valid_qos(config_.qos)) {
        ZEPTO_ERROR("MqttConsumer: invalid QoS {} (must be 0, 1, or 2)",
                      config_.qos);
        return false;
    }

#ifdef ZEPTO_MQTT_AVAILABLE
    if (running_.exchange(true)) return true;  // already running

    try {
        auto* client = new mqtt::async_client(config_.broker_uri,
                                              config_.client_id);

        // Install message callback: Paho delivers messages on an internal
        // worker thread — no separate poll thread is required here.
        client->set_message_callback(
            [this](mqtt::const_message_ptr m) {
                const auto& payload = m->get_payload_ref();
                on_message(payload.data(), payload.size());
            });

        mqtt::connect_options opts;
        opts.set_keep_alive_interval(config_.keepalive_sec);
        opts.set_clean_session(config_.clean_session);
        if (!config_.username.empty()) opts.set_user_name(config_.username);
        if (!config_.password.empty()) opts.set_password(config_.password);

        client->connect(opts)->wait();
        client->subscribe(config_.topic, config_.qos)->wait();

        client_handle_ = client;
        ZEPTO_INFO("MqttConsumer: subscribed to '{}' on '{}' (QoS {})",
                     config_.topic, config_.broker_uri, config_.qos);
        return true;
    } catch (const mqtt::exception& e) {
        ZEPTO_ERROR("MqttConsumer: broker error: {}", e.what());
        running_ = false;
        return false;
    } catch (const std::exception& e) {
        ZEPTO_ERROR("MqttConsumer: {}", e.what());
        running_ = false;
        return false;
    }
#else
    ZEPTO_WARN("MqttConsumer: MQTT support not compiled in "
                 "(rebuild with ZEPTO_USE_MQTT=ON and paho-mqttpp3 installed)");
    return false;
#endif
}

void MqttConsumer::stop() {
    (void)client_handle_;  // suppress unused-field warning when !ZEPTO_MQTT_AVAILABLE
    if (!running_.exchange(false)) return;

#ifdef ZEPTO_MQTT_AVAILABLE
    if (client_handle_) {
        auto* client = static_cast<mqtt::async_client*>(client_handle_);
        try {
            client->unsubscribe(config_.topic)->wait();
            client->disconnect()->wait();  // quiesce Paho callback thread before delete
        } catch (const std::exception& e) {
            ZEPTO_WARN("MqttConsumer: stop error: {}", e.what());
        }
        delete client;
        client_handle_ = nullptr;
    }
#endif
}

// ============================================================================
// Stats
// ============================================================================

MqttStats MqttConsumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

} // namespace zeptodb::feeds

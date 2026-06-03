// ============================================================================
// ZeptoDB: OPC-UA Consumer — implementation
// ============================================================================
// Engine-facing pure logic (NodeId map, variant coercion, 1601→1970
// timestamp, routing with backpressure retry, table_name resolution) is
// fully implemented and exercised by `tests/unit/test_opcua.cpp`.
//
// When built with `-DZEPTO_USE_OPCUA=ON` and open62541 installed,
// start()/stop() wire a real UA_Client session, create a subscription,
// and add a MonitoredItem per config_.nodes (devlog 106, BACKLOG P9 #2b).
// Without the dep, start() refuses with a log message — the same
// behaviour the PoC shipped with in devlog 101.
// ============================================================================

#include "zeptodb/feeds/opcua_consumer.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_set>

#ifdef ZEPTO_OPCUA_AVAILABLE
// open62541 is a C library; its public header wraps its own extern "C".
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/types.h>
#endif

namespace zeptodb::feeds {

// ============================================================================
// Constructor / Destructor
// ============================================================================

OpcUaConsumer::OpcUaConsumer(OpcUaConfig config)
    : config_(std::move(config)) {
    // Build O(1) NodeId → (SymbolId, scale) map eagerly.  Keeping this in
    // the constructor (rather than start()) lets on_data_change() be
    // exercised from unit tests even though start() refuses to run without
    // a license or the open62541 dep.
    node_map_.reserve(config_.nodes.size());
    for (const auto& n : config_.nodes)
        node_map_.emplace(n.node_id, NodeBinding{
            n.symbol_id,
            n.value_scale,
            n.array_symbol_stride == 0 ? 1u : n.array_symbol_stride});
}

OpcUaConsumer::~OpcUaConsumer() {
    stop();
}

// ============================================================================
// Routing setup
// ============================================================================

void OpcUaConsumer::set_pipeline(zeptodb::core::ZeptoPipeline* pipeline) {
    pipeline_ = pipeline;
    if (pipeline_ && !config_.table_name.empty()) {
        uint16_t tid = pipeline_->schema_registry().get_table_id(config_.table_name);
        if (tid == 0) {
            ZEPTO_ERROR("OpcUaConsumer: unknown table '{}' — ticks will be dropped",
                        config_.table_name);
        }
        table_id_ = tid;
    } else {
        table_id_ = 0;
    }
}

void OpcUaConsumer::set_routing(
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
// Dispatch (routing) — byte-identical to MqttConsumer::ingest_decoded.
// Kept local rather than refactored out of mqtt_consumer.cpp to stay
// minimal for the PoC.
//
// Stats atomicity invariant (2r audit, devlog 110):
//   Every outcome below updates exactly ONE counter under a single
//   `lock_guard<std::mutex>(stats_mu_)`. No path increments one field
//   and then decrements another under separate locks, so a concurrent
//   reader's `stats()` snapshot is always self-consistent. The split
//   between `messages_consumed++` (pre-dispatch, in `on_data_change`)
//   and `route_{local,remote}++` / `ingest_failures++` (post-dispatch,
//   here) is semantic — "attempts started" vs "attempts finished" —
//   not a torn transition; a snapshot observing `messages_consumed ==
//   route_local + ingest_failures + 1` is a tick in flight, not a bug.
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

uint32_t stable_symbol_code(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= static_cast<uint32_t>(c);
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

std::string default_server_node_id(uint16_t ns, SymbolId symbol_id) {
    return "ns=" + std::to_string(ns) + ";s=zeptodb.symbol." +
           std::to_string(symbol_id);
}

std::string default_server_display_name(SymbolId symbol_id) {
    return "symbol_" + std::to_string(symbol_id);
}
} // namespace

bool OpcUaConsumer::ingest_decoded(zeptodb::ingestion::TickMessage msg) {
    const int retries  = config_.backpressure_retries;
    const int sleep_us = config_.backpressure_sleep_us;

    if (!config_.table_name.empty() && table_id_ == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.ingest_failures++;
        return false;
    }
    msg.table_id = table_id_;

    if (router_) {
        zeptodb::cluster::NodeId target = router_->route(msg.table_id, msg.symbol_id);
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
// on_data_change: NodeId → TickMessage → dispatch
// ============================================================================
// `status` is the UA_StatusCode reported on the notification (0 = GOOD).
// `config_.quality_handling` decides whether a non-GOOD status drops the
// sample (decode_errors++) or flows through with quality encoded in
// TickMessage.volume.  Devlog 107 (BACKLOG P9 #2j).
bool OpcUaConsumer::on_data_change(const std::string& node_id,
                                   int64_t value,
                                   uint64_t source_ts_ns,
                                   int64_t status) {
    auto it = node_map_.find(node_id);
    if (it == node_map_.end()) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }

    return dispatch_value(it->second.symbol_id, value, source_ts_ns, status,
                          sizeof(int64_t));
}

bool OpcUaConsumer::dispatch_value(SymbolId symbol_id,
                                   int64_t value,
                                   uint64_t source_ts_ns,
                                   int64_t status,
                                   size_t bytes_consumed) {
    if (symbol_id == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }

    const uint32_t sc = static_cast<uint32_t>(status);
    int64_t volume = 0;
    switch (config_.quality_handling) {
        case OpcUaConfig::QualityHandling::IgnoreBad:
            if (sc != 0) {
                std::lock_guard<std::mutex> lk(stats_mu_);
                stats_.decode_errors++;
                return false;
            }
            volume = 0;
            break;
        case OpcUaConfig::QualityHandling::AcceptAll:
            volume = static_cast<int64_t>(sc);  // raw 32-bit status preserved
            break;
        case OpcUaConfig::QualityHandling::AcceptAllGoodAs1:
            volume = (sc == 0) ? 1 : 0;
            break;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.messages_consumed++;
        stats_.bytes_consumed += bytes_consumed;
    }

    zeptodb::ingestion::TickMessage msg{};
    msg.symbol_id = symbol_id;
    msg.price     = value;
    msg.volume    = volume;
    msg.recv_ts   = static_cast<int64_t>(source_ts_ns);
    msg.table_id  = table_id_;  // ingest_decoded will re-stamp, but set now
                                // for callers that bypass dispatch.
    return ingest_decoded(msg);
}

bool OpcUaConsumer::on_array_change(const std::string& node_id,
                                    const std::vector<Variant>& values,
                                    uint64_t source_ts_ns,
                                    int64_t status) {
    auto it = node_map_.find(node_id);
    if (it == node_map_.end() || values.empty()) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }
    const NodeBinding binding = it->second;
    bool all_ok = true;
    for (size_t i = 0; i < values.size(); ++i) {
        const uint64_t offset =
            static_cast<uint64_t>(i) * static_cast<uint64_t>(binding.array_symbol_stride);
        if (offset > std::numeric_limits<SymbolId>::max() ||
            binding.symbol_id > std::numeric_limits<SymbolId>::max() - offset) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.decode_errors++;
            all_ok = false;
            continue;
        }
        int64_t coerced = 0;
        if (!coerce_variant_to_int64(values[i], binding.value_scale, coerced)) {
            on_unsupported_variant();
            all_ok = false;
            continue;
        }
        const SymbolId symbol =
            static_cast<SymbolId>(static_cast<uint64_t>(binding.symbol_id) + offset);
        if (!dispatch_value(symbol, coerced, source_ts_ns, status, sizeof(int64_t))) {
            all_ok = false;
        }
    }
    return all_ok;
}

bool OpcUaConsumer::on_string_change(const std::string& node_id,
                                     const std::string& value,
                                     uint64_t source_ts_ns,
                                     int64_t status) {
    const uint32_t code = pipeline_
        ? pipeline_->symbol_dict().intern(value)
        : stable_symbol_code(value);
    return on_data_change(node_id, static_cast<int64_t>(code),
                          source_ts_ns, status);
}

bool OpcUaConsumer::on_structured_change(
    const std::vector<StructuredField>& fields,
    uint64_t source_ts_ns,
    int64_t status) {
    if (fields.empty()) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }
    bool all_ok = true;
    for (const auto& field : fields) {
        int64_t coerced = 0;
        if (field.symbol_id == 0 ||
            !coerce_variant_to_int64(field.value, field.value_scale, coerced)) {
            on_unsupported_variant();
            all_ok = false;
            continue;
        }
        if (!dispatch_value(field.symbol_id, coerced, source_ts_ns, status,
                            sizeof(int64_t))) {
            all_ok = false;
        }
    }
    return all_ok;
}

size_t OpcUaConsumer::ingest_history(const std::vector<HistoricalSample>& samples) {
    size_t ok_count = 0;
    for (const auto& sample : samples) {
        auto it = node_map_.find(sample.node_id);
        if (it == node_map_.end()) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.decode_errors++;
            continue;
        }
        int64_t coerced = 0;
        if (!coerce_variant_to_int64(sample.value, it->second.value_scale, coerced)) {
            on_unsupported_variant();
            continue;
        }
        if (dispatch_value(it->second.symbol_id, coerced, sample.source_ts_ns,
                           sample.status, sizeof(int64_t))) {
            ++ok_count;
        }
    }
    return ok_count;
}

bool OpcUaConsumer::on_alarm_event(const AlarmEvent& event) {
    if (event.symbol_id == 0) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.decode_errors++;
        return false;
    }
    const int64_t severity = event.active ? event.severity : -event.severity;
    return dispatch_value(event.symbol_id, severity, event.source_ts_ns,
                          event.status, sizeof(int64_t));
}

// ============================================================================
// Lifecycle: start / stop
// ============================================================================
//
// When built with open62541 (ZEPTO_OPCUA_AVAILABLE), start() opens a real
// UA_Client session, creates one subscription, and registers one
// MonitoredItem per config_.nodes entry.  A single background thread
// drives UA_Client_run_iterate() so open62541's own callbacks fire on
// their own thread — on_data_change() is invoked from that thread.
//
// Without the dep, start() returns false — identical to the PoC shipped
// in devlog 101.  The default build (ZEPTO_USE_OPCUA=OFF) is unchanged.
// ============================================================================

#ifdef ZEPTO_OPCUA_AVAILABLE
namespace {

// Per-MonitoredItem context pinned for open62541's C callback.
struct ItemContext {
    OpcUaConsumer* self;
    std::string    node_id;
    double         value_scale;
};

// Per-consumer runtime state kept out of the header to keep the diff
// constrained.  Holds the iterate thread and the allocated item-contexts
// whose addresses open62541 holds as opaque `monContext`.
struct UaRuntime {
    std::thread                                iter_thread;
    std::vector<std::unique_ptr<ItemContext>>  items;
};

static std::mutex                                                     g_rt_mu;
static std::unordered_map<OpcUaConsumer*, std::unique_ptr<UaRuntime>> g_rt;

// Parse "ns=<n>;s=<string>" or "ns=<n>;i=<numeric>".  On success the
// returned UA_NodeId must be UA_NodeId_clear()'d by the caller — string
// NodeIds own heap storage (UA_NODEID_STRING_ALLOC).
bool parse_node_id(const std::string& s, UA_NodeId& out) {
    int ns = 0, consumed = 0;
    if (std::sscanf(s.c_str(), "ns=%d;%n", &ns, &consumed) != 1 || consumed <= 0)
        return false;
    const char* p = s.c_str() + consumed;
    if (p[0] == 'i' && p[1] == '=') {
        unsigned int id = 0;
        if (std::sscanf(p + 2, "%u", &id) != 1) return false;
        out = UA_NODEID_NUMERIC(static_cast<UA_UInt16>(ns), id);
        return true;
    }
    if (p[0] == 's' && p[1] == '=') {
        out = UA_NODEID_STRING_ALLOC(static_cast<UA_UInt16>(ns), p + 2);
        return true;
    }
    return false;
}

// Load a PEM/DER cert/key file into a freshly-allocated UA_ByteString.
// Returns UA_BYTESTRING_NULL on any failure (missing path, unreadable,
// empty); caller bails out with ZEPTO_ERROR.  BACKLOG P9 #2c, devlog 108.
UA_ByteString read_file_to_bytestring(const std::string& path) {
    UA_ByteString out = UA_BYTESTRING_NULL;
    if (path.empty()) return out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    auto len = f.tellg();
    if (len <= 0) return out;
    f.seekg(0, std::ios::beg);
    out.length = static_cast<size_t>(len);
    out.data   = static_cast<UA_Byte*>(UA_malloc(out.length));
    if (!out.data) { out.length = 0; return out; }
    f.read(reinterpret_cast<char*>(out.data), len);
    if (!f) { UA_free(out.data); out.data = nullptr; out.length = 0; }
    return out;
}

bool ua_scalar_to_variant(UA_DataTypeKind kind, const void* data,
                          OpcUaConsumer::Variant& iv) {
    if (!data) return false;
    iv = OpcUaConsumer::Variant{};
    switch (kind) {
        case UA_DATATYPEKIND_INT16:
            iv.type = OpcUaConsumer::VariantType::Int16;
            iv.i64  = *static_cast<const UA_Int16*>(data); break;
        case UA_DATATYPEKIND_INT32:
            iv.type = OpcUaConsumer::VariantType::Int32;
            iv.i64  = *static_cast<const UA_Int32*>(data); break;
        case UA_DATATYPEKIND_INT64:
            iv.type = OpcUaConsumer::VariantType::Int64;
            iv.i64  = *static_cast<const UA_Int64*>(data); break;
        case UA_DATATYPEKIND_UINT16:
            iv.type = OpcUaConsumer::VariantType::Int32;
            iv.i64  = *static_cast<const UA_UInt16*>(data); break;
        case UA_DATATYPEKIND_UINT32:
            iv.type = OpcUaConsumer::VariantType::Int64;
            iv.i64  = *static_cast<const UA_UInt32*>(data); break;
        case UA_DATATYPEKIND_UINT64: {
            UA_UInt64 u = *static_cast<const UA_UInt64*>(data);
            iv.type = OpcUaConsumer::VariantType::Int64;
            iv.i64  = (u > static_cast<UA_UInt64>(std::numeric_limits<int64_t>::max()))
                          ? std::numeric_limits<int64_t>::max()
                          : static_cast<int64_t>(u);
            break;
        }
        case UA_DATATYPEKIND_FLOAT:
            iv.type = OpcUaConsumer::VariantType::Float;
            iv.f64  = *static_cast<const UA_Float*>(data); break;
        case UA_DATATYPEKIND_DOUBLE:
            iv.type = OpcUaConsumer::VariantType::Double;
            iv.f64  = *static_cast<const UA_Double*>(data); break;
        case UA_DATATYPEKIND_BOOLEAN:
            iv.type = OpcUaConsumer::VariantType::Boolean;
            iv.b    = *static_cast<const UA_Boolean*>(data); break;
        default:
            return false;
    }
    return true;
}

std::string ua_string_to_std(const UA_String& s) {
    if (!s.data || s.length == 0) return {};
    return std::string(reinterpret_cast<const char*>(s.data), s.length);
}

// Coerce a UA_Variant via the already-unit-tested
// OpcUaConsumer::coerce_variant_to_int64() so the float-clamp /
// NaN-reject logic (devlog 105, BACKLOG P9 #2n) stays in one place.
bool variant_to_int64(const UA_Variant& v, double scale, int64_t& out) {
    if (!v.type || !UA_Variant_isScalar(&v) || !v.data) return false;
    OpcUaConsumer::Variant iv{};
    if (!ua_scalar_to_variant(v.type->typeKind, v.data, iv)) return false;
    return OpcUaConsumer::coerce_variant_to_int64(iv, scale, out);
}

// Data-change handler — tiny bridge between open62541's C callback and
// OpcUaConsumer dispatch. Source-TS preferred, Server-TS fallback, host
// wall-clock fallback. Returning bool lets the Historical Access adapter reuse
// the exact same decode/quality/routing path.
bool handle_data_change(OpcUaConsumer* self, const ItemContext& ctx,
                        const UA_DataValue* value) {
    if (!self || !value || !value->hasValue) return false;

    uint64_t ts_ns;
    if (value->hasSourceTimestamp) {
        ts_ns = OpcUaConsumer::ua_datetime_to_ns(
            static_cast<int64_t>(value->sourceTimestamp));
    } else if (value->hasServerTimestamp) {
        ts_ns = OpcUaConsumer::ua_datetime_to_ns(
            static_cast<int64_t>(value->serverTimestamp));
    } else {
        ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    const UA_Variant& variant = value->value;
    if (variant.type && variant.type->typeKind == UA_DATATYPEKIND_STRING &&
        UA_Variant_isScalar(&variant) && variant.data) {
        return self->on_string_change(ctx.node_id,
            ua_string_to_std(*static_cast<const UA_String*>(variant.data)),
            ts_ns, value->hasStatus ? static_cast<int64_t>(value->status) : 0);
    }
    if (variant.type && !UA_Variant_isScalar(&variant) && variant.data &&
        variant.arrayLength > 0 && variant.type->memSize > 0) {
        std::vector<OpcUaConsumer::Variant> values;
        values.reserve(variant.arrayLength);
        const auto* base = static_cast<const uint8_t*>(variant.data);
        for (size_t i = 0; i < variant.arrayLength; ++i) {
            OpcUaConsumer::Variant iv{};
            if (!ua_scalar_to_variant(variant.type->typeKind,
                                      base + (i * variant.type->memSize), iv)) {
                self->on_unsupported_variant();
                return;
            }
            values.push_back(iv);
        }
        return self->on_array_change(ctx.node_id, values, ts_ns,
            value->hasStatus ? static_cast<int64_t>(value->status) : 0);
    }

    int64_t coerced = 0;
    if (!variant_to_int64(variant, ctx.value_scale, coerced)) {
        // Unsupported variant type → explicit decode_errors bump, no
        // dispatch.  Sprint-2 polish 2 (devlog 110) replaced an earlier
        // empty-node_id piggy-back that coupled two different error
        // semantics onto the same counter path.
        self->on_unsupported_variant();
        return false;
    }
    const int64_t status =
        value->hasStatus ? static_cast<int64_t>(value->status) : 0;
    return self->on_data_change(ctx.node_id, coerced, ts_ns, status);
}

extern "C" void ua_data_change_cb(UA_Client* /*client*/,
                                  UA_UInt32  /*subId*/,
                                  void*      /*subContext*/,
                                  UA_UInt32  /*monId*/,
                                  void*      monContext,
                                  UA_DataValue* value) {
    auto* ctx = static_cast<ItemContext*>(monContext);
    if (!ctx) return;
    (void)handle_data_change(ctx->self, *ctx, value);
}

#ifdef UA_ENABLE_HISTORIZING
struct HistoryReadContext {
    OpcUaConsumer* self = nullptr;
    ItemContext item;
    size_t routed = 0;
};

UA_Boolean ua_history_read_cb(UA_Client* /*client*/,
                              const UA_NodeId* /*nodeId*/,
                              UA_Boolean /*moreDataAvailable*/,
                              const UA_ExtensionObject* data,
                              void* callbackContext) {
    auto* ctx = static_cast<HistoryReadContext*>(callbackContext);
    if (!ctx || !ctx->self || !data) return UA_FALSE;
    if (data->encoding != UA_EXTENSIONOBJECT_DECODED ||
        !data->content.decoded.type ||
        data->content.decoded.type != &UA_TYPES[UA_TYPES_HISTORYDATA] ||
        !data->content.decoded.data) {
        ctx->self->on_unsupported_variant();
        return UA_FALSE;
    }

    const auto* history =
        static_cast<const UA_HistoryData*>(data->content.decoded.data);
    for (size_t i = 0; i < history->dataValuesSize; ++i) {
        if (handle_data_change(ctx->self, ctx->item, &history->dataValues[i])) {
            ++ctx->routed;
        }
    }
    return UA_TRUE;
}
#endif  // UA_ENABLE_HISTORIZING

struct UaServerRuntime {
    std::thread iter_thread;
};

static std::mutex                                                    g_server_rt_mu;
static std::unordered_map<OpcUaServer*, std::unique_ptr<UaServerRuntime>> g_server_rt;

uint16_t endpoint_port_or_default(const std::string& endpoint) {
    const size_t pos = endpoint.rfind(':');
    if (pos == std::string::npos || pos + 1 >= endpoint.size()) return 4840;
    char* end = nullptr;
    const unsigned long port = std::strtoul(endpoint.c_str() + pos + 1, &end, 10);
    if (!end || *end != '\0' || port == 0 || port > 65535) return 4840;
    return static_cast<uint16_t>(port);
}

} // namespace
#endif  // ZEPTO_OPCUA_AVAILABLE

size_t OpcUaConsumer::read_history(const HistoryReadOptions& options) {
#if defined(ZEPTO_OPCUA_AVAILABLE) && defined(UA_ENABLE_HISTORIZING)
    auto* client = static_cast<UA_Client*>(client_handle_);
    if (!client || config_.nodes.empty()) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.ingest_failures++;
        return 0;
    }

    size_t routed = 0;
    for (const auto& n : config_.nodes) {
        UA_NodeId nid;
        if (!parse_node_id(n.node_id, nid)) {
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.decode_errors++;
            continue;
        }

        HistoryReadContext ctx;
        ctx.self = this;
        ctx.item = ItemContext{this, n.node_id, n.value_scale};
        const UA_StatusCode sc = UA_Client_HistoryRead_raw(
            client,
            &nid,
            ua_history_read_cb,
            ns_to_ua_datetime(options.start_ts_ns),
            ns_to_ua_datetime(options.end_ts_ns),
            UA_STRING_NULL,
            options.return_bounds ? UA_TRUE : UA_FALSE,
            options.values_per_node,
            UA_TIMESTAMPSTORETURN_BOTH,
            &ctx);
        UA_NodeId_clear(&nid);

        if (sc != UA_STATUSCODE_GOOD) {
            ZEPTO_WARN("OpcUaConsumer: HistoryRead_raw failed for '{}': 0x{:08x}",
                       n.node_id, static_cast<uint32_t>(sc));
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.ingest_failures++;
            continue;
        }
        routed += ctx.routed;
    }
    return routed;
#else
    (void)options;
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.ingest_failures++;
    ZEPTO_WARN("OpcUaConsumer: Historical Access read requires open62541 with UA_ENABLE_HISTORIZING");
    return 0;
#endif
}

bool OpcUaConsumer::start() {
    // Config validation first — fail fast before touching licensing or the
    // protocol library so tests and misconfigurations surface the real cause.
    if (config_.endpoint.empty()) {
        ZEPTO_ERROR("OpcUaConsumer: config.endpoint is empty");
        return false;
    }
    if (config_.nodes.empty()) {
        ZEPTO_ERROR("OpcUaConsumer: config.nodes is empty — nothing to subscribe to");
        return false;
    }
    {
        std::unordered_set<std::string> seen;
        for (const auto& n : config_.nodes) {
            if (n.node_id.empty()) {
                ZEPTO_ERROR("OpcUaConsumer: config.nodes contains empty node_id");
                return false;
            }
            if (!seen.insert(n.node_id).second) {
                ZEPTO_ERROR("OpcUaConsumer: config.nodes contains duplicate node_id '{}'",
                            n.node_id);
                return false;
            }
        }
    }
    if (!is_valid_security(config_.security_mode, config_.security_policy)) {
        ZEPTO_ERROR("OpcUaConsumer: invalid security combo (mode={}, policy={})",
                    static_cast<int>(config_.security_mode),
                    static_cast<int>(config_.security_policy));
        return false;
    }
    // Sign / SignAndEncrypt require a client cert + key on disk.  Checked
    // here (not in is_valid_security) to keep that helper's signature
    // unchanged.  BACKLOG P9 #2c.
    if (config_.security_mode == OpcUaConfig::SecurityMode::Sign ||
        config_.security_mode == OpcUaConfig::SecurityMode::SignAndEncrypt) {
        if (config_.client_cert_path.empty() || config_.client_key_path.empty()) {
            ZEPTO_ERROR("OpcUaConsumer: Sign/SignAndEncrypt requires client_cert_path and client_key_path");
            return false;
        }
    }
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("OPC-UA consumer requires Enterprise license (IOT_CONNECTORS) — not starting");
        return false;
    }

#ifdef ZEPTO_OPCUA_AVAILABLE
    // ---- 1. Create UA_Client + apply defaults ----------------------------
    UA_Client* client = UA_Client_new();
    if (!client) {
        ZEPTO_ERROR("OpcUaConsumer: UA_Client_new() failed");
        return false;
    }
    UA_ClientConfig* cc = UA_Client_getConfig(client);
    if (config_.security_mode == OpcUaConfig::SecurityMode::None &&
        config_.security_policy == OpcUaConfig::SecurityPolicy::None) {
        UA_ClientConfig_setDefault(cc);
    } else {
        // Basic256Sha256 with certificate-based Sign / SignAndEncrypt.
        // Revocation list and multi-cert CA chain are not supported in
        // MVP; trust list is at most the single configured server cert.
        // BACKLOG P9 #2c, devlog 108.
        UA_ByteString cert_blob = read_file_to_bytestring(config_.client_cert_path);
        UA_ByteString key_blob  = read_file_to_bytestring(config_.client_key_path);
        if (cert_blob.length == 0 || key_blob.length == 0) {
            ZEPTO_ERROR("OpcUaConsumer: failed to read cert ({}) or key ({})",
                        config_.client_cert_path, config_.client_key_path);
            UA_ByteString_clear(&cert_blob);
            UA_ByteString_clear(&key_blob);
            UA_Client_delete(client);
            return false;
        }

        UA_ByteString trust_list[1];
        size_t trust_count = 0;
        if (!config_.server_cert_path.empty()) {
            trust_list[0] = read_file_to_bytestring(config_.server_cert_path);
            trust_count = trust_list[0].length > 0 ? 1 : 0;
        }

        UA_StatusCode enc_sc = UA_ClientConfig_setDefaultEncryption(
            cc, cert_blob, key_blob,
            trust_count > 0 ? trust_list : nullptr, trust_count,
            nullptr, 0);

        UA_ByteString_clear(&cert_blob);
        UA_ByteString_clear(&key_blob);
        if (trust_count > 0) UA_ByteString_clear(&trust_list[0]);

        if (enc_sc != UA_STATUSCODE_GOOD) {
            ZEPTO_ERROR("OpcUaConsumer: UA_ClientConfig_setDefaultEncryption failed: {}",
                        UA_StatusCode_name(enc_sc));
            UA_Client_delete(client);
            return false;
        }

        // setDefaultEncryption installed the Basic256Sha256 policy URI;
        // we only need to pick between Sign and SignAndEncrypt here.
        cc->securityMode = (config_.security_mode == OpcUaConfig::SecurityMode::Sign)
            ? UA_MESSAGESECURITYMODE_SIGN
            : UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
    }
    cc->timeout                  = config_.connect_timeout_ms;   // ms
    cc->requestedSessionTimeout  = config_.session_timeout_ms;   // ms

    // ---- 2. Connect ------------------------------------------------------
    UA_StatusCode sc;
    if (!config_.username.empty()) {
        sc = UA_Client_connectUsername(client, config_.endpoint.c_str(),
                                       config_.username.c_str(),
                                       config_.password.c_str());
    } else {
        sc = UA_Client_connect(client, config_.endpoint.c_str());
    }
    if (sc != UA_STATUSCODE_GOOD) {
        ZEPTO_ERROR("OpcUaConsumer: connect failed ({}): 0x{:08x}",
                    config_.endpoint, static_cast<uint32_t>(sc));
        UA_Client_delete(client);
        return false;
    }

    // ---- 3. Create subscription + MonitoredItems + start publish loop ---
    client_handle_ = client;
    {
        std::lock_guard<std::mutex> lk(g_rt_mu);
        g_rt[this] = std::make_unique<UaRuntime>();
    }
    if (!setup_subscription()) {
        std::lock_guard<std::mutex> lk(g_rt_mu);
        g_rt.erase(this);
        UA_Client_disconnect(client);
        UA_Client_delete(client);
        client_handle_ = nullptr;
        return false;
    }

    running_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_rt_mu);
        g_rt[this]->iter_thread = std::thread([this] { run_iterate_loop(); });
    }
    ZEPTO_INFO("OpcUaConsumer started: endpoint={}, subscription_id={}, items={}",
               config_.endpoint, subscription_id_, config_.nodes.size());
    return true;
#else
    ZEPTO_WARN("OpcUaConsumer: OPC-UA support not compiled in "
               "(rebuild with ZEPTO_USE_OPCUA=ON and open62541-devel installed)");
    return false;
#endif
}

void OpcUaConsumer::stop() {
    (void)client_handle_;     // suppress unused-field warning in !ZEPTO_OPCUA_AVAILABLE
    (void)subscription_id_;
    if (!running_.exchange(false)) return;

#ifdef ZEPTO_OPCUA_AVAILABLE
    std::unique_ptr<UaRuntime> rt;
    {
        std::lock_guard<std::mutex> lk(g_rt_mu);
        auto it = g_rt.find(this);
        if (it != g_rt.end()) { rt = std::move(it->second); g_rt.erase(it); }
    }
    if (rt && rt->iter_thread.joinable()) rt->iter_thread.join();

    if (client_handle_) {
        auto* client = static_cast<UA_Client*>(client_handle_);
        if (subscription_id_ != 0) {
            UA_Client_Subscriptions_deleteSingle(client, subscription_id_);
        }
        UA_Client_disconnect(client);
        UA_Client_delete(client);
        client_handle_ = nullptr;
    }
    subscription_id_ = 0;
    ZEPTO_INFO("OpcUaConsumer stopped: endpoint={}", config_.endpoint);
#endif
}

// ============================================================================
// Subscription setup (shared by initial connect and reconnect)
// ============================================================================
// Creates a fresh UA subscription on the current UA_Client and re-adds
// one MonitoredItem per config_.nodes.  On reconnect the old subscription
// died with the session, so a full rebuild is correct and simpler than
// trying to reuse server-side state.  BACKLOG P9 #2i, devlog 109.
bool OpcUaConsumer::setup_subscription() {
#ifdef ZEPTO_OPCUA_AVAILABLE
    auto* client = static_cast<UA_Client*>(client_handle_);
    if (!client) return false;

    UA_CreateSubscriptionRequest  sreq  = UA_CreateSubscriptionRequest_default();
    sreq.requestedPublishingInterval    = config_.publishing_interval_ms;
    UA_CreateSubscriptionResponse sresp = UA_Client_Subscriptions_create(
        client, sreq, nullptr, nullptr, nullptr);
    if (sresp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        ZEPTO_ERROR("OpcUaConsumer: CreateSubscription failed: 0x{:08x}",
                    static_cast<uint32_t>(sresp.responseHeader.serviceResult));
        return false;
    }
    subscription_id_ = sresp.subscriptionId;

    std::vector<std::unique_ptr<ItemContext>> items;
    items.reserve(config_.nodes.size());
    for (const auto& n : config_.nodes) {
        UA_NodeId nid;
        if (!parse_node_id(n.node_id, nid)) {
            ZEPTO_WARN("OpcUaConsumer: unparseable node_id '{}', skipping", n.node_id);
            { std::lock_guard<std::mutex> lk(stats_mu_); stats_.decode_errors++; }
            continue;
        }
        auto ctx = std::make_unique<ItemContext>(
            ItemContext{this, n.node_id, n.value_scale});

        UA_MonitoredItemCreateRequest mreq =
            UA_MonitoredItemCreateRequest_default(nid);
        mreq.requestedParameters.samplingInterval = config_.sampling_interval_ms;
        mreq.requestedParameters.queueSize        = config_.queue_size;
        mreq.requestedParameters.discardOldest    = config_.discard_oldest;

        UA_MonitoredItemCreateResult mres =
            UA_Client_MonitoredItems_createDataChange(
                client, subscription_id_, UA_TIMESTAMPSTORETURN_BOTH,
                mreq, ctx.get(), ua_data_change_cb, nullptr);
        UA_NodeId_clear(&nid);

        if (mres.statusCode != UA_STATUSCODE_GOOD) {
            ZEPTO_WARN("OpcUaConsumer: CreateMonitoredItem failed for '{}': 0x{:08x}",
                       n.node_id, static_cast<uint32_t>(mres.statusCode));
            { std::lock_guard<std::mutex> lk(stats_mu_); stats_.decode_errors++; }
            continue;
        }
        items.push_back(std::move(ctx));
    }

    std::lock_guard<std::mutex> lk(g_rt_mu);
    auto it = g_rt.find(this);
    if (it == g_rt.end()) return false;  // start() should have created it
    it->second->items = std::move(items);
    return true;
#else
    return false;
#endif
}

// ============================================================================
// Background publish-loop with jittered exponential-backoff reconnect
// ============================================================================
// open62541 surfaces connection loss via the status code returned from
// UA_Client_run_iterate().  On a Bad*Closed / BadServerNotConnected
// result we sleep `reconnect_interval_ms` ± 25% jitter and retry
// UA_Client_connect(); on failure the backoff doubles up to 16× the
// base (≈ 32 s with the default 2 s base).  Successful reconnects bump
// OpcUaStats::reconnects and rebuild the subscription.  BACKLOG P9 #2i,
// devlog 109.
void OpcUaConsumer::run_iterate_loop() {
#ifdef ZEPTO_OPCUA_AVAILABLE
    auto* client = static_cast<UA_Client*>(client_handle_);
    uint32_t backoff_ms     = config_.reconnect_interval_ms;
    const uint32_t max_backoff_ms = config_.reconnect_interval_ms * 16;

    while (running_.load(std::memory_order_acquire)) {
        UA_StatusCode sc = UA_Client_run_iterate(client, 100);
        if (sc != UA_STATUSCODE_BADCONNECTIONCLOSED &&
            sc != UA_STATUSCODE_BADSERVERNOTCONNECTED &&
            sc != UA_STATUSCODE_BADSECURECHANNELCLOSED) {
            continue;
        }
        ZEPTO_WARN("OpcUaConsumer: connection lost ({}); reconnecting in {} ms",
                   UA_StatusCode_name(sc), backoff_ms);

        // Jitter ±25% of current backoff.
        const uint32_t jitter   = backoff_ms / 4;
        const uint32_t jrange   = 2 * jitter + 1;
        const uint32_t sleep_ms = backoff_ms - jitter +
            static_cast<uint32_t>(std::rand()) % jrange;
        for (uint32_t waited = 0;
             waited < sleep_ms && running_.load(std::memory_order_acquire);
             waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!running_.load(std::memory_order_acquire)) break;

        // UA_Client_connect on an existing UA_Client reuses its ClientConfig
        // (including any Basic256Sha256 setup installed in start()), so we
        // don't re-prime encryption here.  BACKLOG P9 #2c, devlog 108.
        UA_StatusCode rc = UA_Client_connect(client, config_.endpoint.c_str());
        if (rc != UA_STATUSCODE_GOOD) {
            backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
            continue;
        }
        ZEPTO_INFO("OpcUaConsumer: reconnected to {}", config_.endpoint);
        backoff_ms = config_.reconnect_interval_ms;  // reset on success

        if (!setup_subscription()) {
            ZEPTO_WARN("OpcUaConsumer: re-subscription failed; will retry");
            // Next iterate() will see the dead state and loop us back.
        }
        { std::lock_guard<std::mutex> lk(stats_mu_); stats_.reconnects++; }
    }
#endif
}

// ============================================================================
// Stats
// ============================================================================

OpcUaStats OpcUaConsumer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

// Explicit single-field transition: an unsupported UA variant raises
// decode_errors and never dispatches. One critical section, one field —
// atomic w.r.t. any reader snapshot (2r audit, devlog 110).
void OpcUaConsumer::on_unsupported_variant() {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.decode_errors++;
}

// ============================================================================
// Static helpers
// ============================================================================

bool OpcUaConsumer::coerce_variant_to_int64(const Variant& v,
                                            double value_scale,
                                            int64_t& out) {
    switch (v.type) {
        case VariantType::Int16:
        case VariantType::Int32:
        case VariantType::Int64:
            out = v.i64;
            return true;
        case VariantType::Float:
        case VariantType::Double: {
            const double scaled = v.f64 * value_scale;
            if (!std::isfinite(scaled)) return false;  // NaN / ±Inf
            if (scaled >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
                out = std::numeric_limits<int64_t>::max();
                return true;
            }
            if (scaled <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
                out = std::numeric_limits<int64_t>::min();
                return true;
            }
            out = static_cast<int64_t>(scaled);
            return true;
        }
        case VariantType::Boolean:
            out = v.b ? 1 : 0;
            return true;
        case VariantType::Unsupported:
        default:
            return false;
    }
}

uint64_t OpcUaConsumer::ua_datetime_to_ns(int64_t ua_datetime_100ns_since_1601) {
    // UA DateTime = 100 ns ticks since 1601-01-01 UTC.
    // Unix epoch (1970-01-01 UTC) = 116444736000000000 × 100 ns ticks later.
    constexpr int64_t kEpochOffset100ns = 116444736000000000LL;
    if (ua_datetime_100ns_since_1601 < kEpochOffset100ns) return 0;
    return static_cast<uint64_t>(
        (ua_datetime_100ns_since_1601 - kEpochOffset100ns) * 100LL);
}

int64_t OpcUaConsumer::ns_to_ua_datetime(uint64_t unix_ns) {
    constexpr int64_t kEpochOffset100ns = 116444736000000000LL;
    constexpr uint64_t kMaxDelta100ns =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max() -
                              kEpochOffset100ns);
    const uint64_t delta100ns = unix_ns / 100ULL;
    if (delta100ns >= kMaxDelta100ns) {
        return std::numeric_limits<int64_t>::max();
    }
    return kEpochOffset100ns + static_cast<int64_t>(delta100ns);
}

bool OpcUaConsumer::is_valid_security(OpcUaConfig::SecurityMode mode,
                                      OpcUaConfig::SecurityPolicy policy) {
    // Sign or SignAndEncrypt require a real security policy (not None).
    if (mode != OpcUaConfig::SecurityMode::None &&
        policy == OpcUaConfig::SecurityPolicy::None) {
        return false;
    }
    // None/None is a valid dev-only combo.
    return true;
}

// ============================================================================
// Sector-aware default profiles (BACKLOG P9 #2q)
// ============================================================================
void OpcUaConfig::apply_profile(Profile p) {
    switch (p) {
        case Profile::Generic:
            break;
        case Profile::Fab:
            queue_size             = 1000;
            sampling_interval_ms   = 0.1;
            publishing_interval_ms = 10.0;
            backpressure_retries   = 20;
            break;
        case Profile::Auto:
            queue_size             = 100;
            sampling_interval_ms   = 1.0;
            publishing_interval_ms = 10.0;
            backpressure_retries   = 10;
            break;
        case Profile::Steel:
            queue_size             = 100;
            sampling_interval_ms   = 0.2;
            publishing_interval_ms = 10.0;
            backpressure_retries   = 10;
            break;
    }
}

// ============================================================================
// OpcUaServer
// ============================================================================

OpcUaServer::OpcUaServer(OpcUaServerConfig config)
    : config_(std::move(config)) {
    for (const auto& node : config_.nodes) {
        if (node.symbol_id == 0) continue;
        NodeState state;
        state.node_id = node.node_id.empty()
            ? default_server_node_id(config_.namespace_index, node.symbol_id)
            : node.node_id;
        state.display_name = node.display_name.empty()
            ? default_server_display_name(node.symbol_id)
            : node.display_name;
        state.value = node.initial_value;
        values_.emplace(node.symbol_id, std::move(state));
    }
}

OpcUaServer::~OpcUaServer() {
    stop();
}

void OpcUaServer::bump_start_failure() {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.start_failures++;
}

void OpcUaServer::bump_write_failure() {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.write_failures++;
}

bool OpcUaServer::start() {
    if (config_.endpoint.empty() || config_.nodes.empty()) {
        bump_start_failure();
        return false;
    }
    {
        std::unordered_set<SymbolId> symbols;
        std::unordered_set<std::string> node_ids;
        for (const auto& node : config_.nodes) {
            if (node.symbol_id == 0 || !symbols.insert(node.symbol_id).second) {
                bump_start_failure();
                return false;
            }
            const std::string node_id = node.node_id.empty()
                ? default_server_node_id(config_.namespace_index, node.symbol_id)
                : node.node_id;
            if (node_id.empty() || !node_ids.insert(node_id).second) {
                bump_start_failure();
                return false;
            }
        }
    }

#ifdef ZEPTO_OPCUA_AVAILABLE
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::IOT_CONNECTORS)) {
        ZEPTO_WARN("OPC-UA server mode requires Enterprise license (IOT_CONNECTORS) — not starting");
        bump_start_failure();
        return false;
    }

    UA_Server* server = UA_Server_new();
    if (!server) {
        bump_start_failure();
        return false;
    }

    const uint16_t port = endpoint_port_or_default(config_.endpoint);
    UA_StatusCode sc = UA_ServerConfig_setMinimal(
        UA_Server_getConfig(server), port, nullptr);
    if (sc != UA_STATUSCODE_GOOD) {
        ZEPTO_ERROR("OpcUaServer: UA_ServerConfig_setMinimal failed: 0x{:08x}",
                    static_cast<uint32_t>(sc));
        UA_Server_delete(server);
        bump_start_failure();
        return false;
    }

    for (const auto& entry : values_) {
        const SymbolId symbol_id = entry.first;
        const NodeState& state = entry.second;

        UA_NodeId nid;
        if (!parse_node_id(state.node_id, nid)) {
            ZEPTO_ERROR("OpcUaServer: unsupported node_id '{}'", state.node_id);
            UA_Server_delete(server);
            bump_start_failure();
            return false;
        }

        UA_VariableAttributes attr = UA_VariableAttributes_default;
        UA_Int64 initial = static_cast<UA_Int64>(state.value);
        UA_Variant_setScalar(&attr.value, &initial, &UA_TYPES[UA_TYPES_INT64]);
        attr.dataType = UA_TYPES[UA_TYPES_INT64].typeId;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.displayName = UA_LOCALIZEDTEXT_ALLOC(
            "en-US", state.display_name.c_str());

        UA_QualifiedName browse_name = UA_QUALIFIEDNAME_ALLOC(
            nid.namespaceIndex, state.display_name.c_str());
        const UA_StatusCode add_sc = UA_Server_addVariableNode(
            server,
            nid,
            UA_NS0ID(OBJECTSFOLDER),
            UA_NS0ID(ORGANIZES),
            browse_name,
            UA_NS0ID(BASEDATAVARIABLETYPE),
            attr,
            nullptr,
            nullptr);
        UA_QualifiedName_clear(&browse_name);
        UA_VariableAttributes_clear(&attr);
        UA_NodeId_clear(&nid);

        if (add_sc != UA_STATUSCODE_GOOD) {
            ZEPTO_ERROR("OpcUaServer: add variable for symbol {} failed: 0x{:08x}",
                        symbol_id, static_cast<uint32_t>(add_sc));
            UA_Server_delete(server);
            bump_start_failure();
            return false;
        }
    }

    sc = UA_Server_run_startup(server);
    if (sc != UA_STATUSCODE_GOOD) {
        ZEPTO_ERROR("OpcUaServer: run_startup failed: 0x{:08x}",
                    static_cast<uint32_t>(sc));
        UA_Server_delete(server);
        bump_start_failure();
        return false;
    }

    server_handle_ = server;
    running_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_server_rt_mu);
        auto rt = std::make_unique<UaServerRuntime>();
        rt->iter_thread = std::thread([this, server] {
            while (running_.load(std::memory_order_acquire)) {
                (void)UA_Server_run_iterate(server, UA_FALSE);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.iterate_sleep_ms));
            }
        });
        g_server_rt[this] = std::move(rt);
    }
    ZEPTO_INFO("OpcUaServer started: endpoint={}, nodes={}",
               config_.endpoint, values_.size());
    return true;
#else
    ZEPTO_WARN("OpcUaServer: OPC-UA support not compiled in "
               "(rebuild with ZEPTO_USE_OPCUA=ON and open62541-devel installed)");
    bump_start_failure();
    return false;
#endif
}

void OpcUaServer::stop() {
    (void)server_handle_;
    if (!running_.exchange(false)) return;

#ifdef ZEPTO_OPCUA_AVAILABLE
    std::unique_ptr<UaServerRuntime> rt;
    {
        std::lock_guard<std::mutex> lk(g_server_rt_mu);
        auto it = g_server_rt.find(this);
        if (it != g_server_rt.end()) {
            rt = std::move(it->second);
            g_server_rt.erase(it);
        }
    }
    if (rt && rt->iter_thread.joinable()) rt->iter_thread.join();

    if (server_handle_) {
        auto* server = static_cast<UA_Server*>(server_handle_);
        (void)UA_Server_run_shutdown(server);
        UA_Server_delete(server);
        server_handle_ = nullptr;
    }
    ZEPTO_INFO("OpcUaServer stopped: endpoint={}", config_.endpoint);
#endif
}

bool OpcUaServer::publish_value(SymbolId symbol_id,
                                int64_t value,
                                uint64_t source_ts_ns,
                                int64_t status) {
    NodeState state;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = values_.find(symbol_id);
        if (it == values_.end()) {
            bump_write_failure();
            return false;
        }
        it->second.value = value;
        it->second.source_ts_ns = source_ts_ns;
        it->second.status = status;
        state = it->second;
    }

    if (running_.load(std::memory_order_acquire) &&
        !write_live_value(symbol_id, state)) {
        bump_write_failure();
        return false;
    }

    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.writes++;
    return true;
}

std::optional<int64_t> OpcUaServer::snapshot_value(SymbolId symbol_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = values_.find(symbol_id);
    if (it == values_.end()) return std::nullopt;
    return it->second.value;
}

OpcUaServerStats OpcUaServer::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

bool OpcUaServer::write_live_value(SymbolId /*symbol_id*/, const NodeState& state) {
#ifdef ZEPTO_OPCUA_AVAILABLE
    auto* server = static_cast<UA_Server*>(server_handle_);
    if (!server) return false;

    UA_NodeId nid;
    if (!parse_node_id(state.node_id, nid)) return false;

    UA_Int64 value = static_cast<UA_Int64>(state.value);
    UA_WriteValue write;
    UA_WriteValue_init(&write);
    write.nodeId = nid;
    write.attributeId = UA_ATTRIBUTEID_VALUE;
    write.value.hasValue = UA_TRUE;
    UA_Variant_setScalar(&write.value.value, &value, &UA_TYPES[UA_TYPES_INT64]);
    write.value.hasStatus = UA_TRUE;
    write.value.status = static_cast<UA_StatusCode>(state.status);
    if (state.source_ts_ns != 0) {
        write.value.hasSourceTimestamp = UA_TRUE;
        write.value.sourceTimestamp =
            OpcUaConsumer::ns_to_ua_datetime(state.source_ts_ns);
    }

    const UA_StatusCode sc = UA_Server_write(server, &write);
    UA_NodeId_clear(&nid);
    return sc == UA_STATUSCODE_GOOD;
#else
    (void)state;
    return false;
#endif
}

} // namespace zeptodb::feeds

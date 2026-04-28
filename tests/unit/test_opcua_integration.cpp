// ============================================================================
// ZeptoDB: OPC-UA Consumer — live integration test (BACKLOG P9 #2k, devlog 107)
// ============================================================================
// Spins up open62541's bundled tutorial-style server in-process, exposes a
// single Int32 node `ns=1;s=the.answer` (value 42), then connects an
// OpcUaConsumer and asserts at least one tick lands in the pipeline.
//
// This file compiles to nothing unless BOTH the OPC-UA consumer was enabled
// (`-DZEPTO_USE_OPCUA=ON`) AND the open62541 development headers are
// installed at build time.  CI must install `open62541-devel` (RHEL/AL2)
// or `libopen62541-dev` (Debian/Ubuntu) for this test to run.  Same silent
// optional-dep pattern used by ZEPTO_MQTT_AVAILABLE / ZEPTO_KAFKA_AVAILABLE.
// ============================================================================

#ifdef ZEPTO_OPCUA_AVAILABLE

#include "zeptodb/feeds/opcua_consumer.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/auth/license_validator.h"

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace zeptodb::feeds;

namespace {

// All-features Enterprise license (bits 0..15 set) — IOT_CONNECTORS is bit 8.
void ensure_enterprise_license() {
    static bool loaded = false;
    if (loaded) return;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string payload = R"({"edition":"enterprise","features":65535,"max_nodes":64,"exp":)" +
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
    loaded = true;
}

// In-process minimal OPC-UA server — mirrors open62541's
// `tutorial_server_variable` example, scoped to one Int32 node so the test
// stays self-contained.
static UA_Server*            g_test_server = nullptr;
static std::atomic<bool>     g_server_running{false};

void start_tutorial_server(uint16_t port) {
    g_test_server = UA_Server_new();
    UA_ServerConfig_setMinimal(UA_Server_getConfig(g_test_server), port, nullptr);

    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Int32 answer = 42;
    UA_Variant_setScalar(&attr.value, &answer, &UA_TYPES[UA_TYPES_INT32]);
    attr.description = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                        const_cast<char*>("the answer"));
    attr.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                        const_cast<char*>("the answer"));
    UA_NodeId nodeId = UA_NODEID_STRING(1, const_cast<char*>("the.answer"));
    UA_Server_addVariableNode(
        g_test_server, nodeId,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, const_cast<char*>("the.answer")),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        attr, nullptr, nullptr);

    g_server_running = true;
    UA_Server_run(g_test_server, reinterpret_cast<volatile UA_Boolean*>(&g_server_running));
    UA_Server_delete(g_test_server);
    g_test_server = nullptr;
}

} // namespace

// ============================================================================
// End-to-end: connect, subscribe, receive at least one tick, shut down.
// ============================================================================
TEST(OpcUaIntegration, ConnectsAndReceivesTickFromLiveServer) {
    // Port in the 14840 range — well outside the ephemeral range used by
    // test_cluster.cpp's PORT_OFF, so parallel ctest jobs won't collide on
    // the common dev defaults.
    const uint16_t port = 14840;
    std::thread server_thread(start_tutorial_server, port);
    // Give the server a moment to open its listening socket.
    std::this_thread::sleep_for(500ms);

    ensure_enterprise_license();

    OpcUaConfig cfg;
    cfg.endpoint = "opc.tcp://127.0.0.1:" + std::to_string(port);
    cfg.nodes.push_back({"ns=1;s=the.answer", 1, 1.0});
    cfg.publishing_interval_ms = 100.0;
    cfg.sampling_interval_ms   = 50.0;
    cfg.table_name             = "trades";

    zeptodb::core::PipelineConfig pc;
    pc.storage_mode = zeptodb::core::StorageMode::PURE_IN_MEMORY;
    auto pipeline = std::make_unique<zeptodb::core::ZeptoPipeline>(pc);
    pipeline->start();

    zeptodb::sql::QueryExecutor ex(*pipeline);
    ex.execute("CREATE TABLE trades (symbol INT64, price INT64, volume INT64, "
               "timestamp TIMESTAMP_NS)");

    OpcUaConsumer consumer(cfg);
    consumer.set_pipeline(pipeline.get());
    ASSERT_TRUE(consumer.start());

    // Poll up to ~5 s for the first notification to arrive.
    for (int i = 0; i < 50; ++i) {
        if (consumer.stats().messages_consumed >= 1) break;
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_GE(consumer.stats().messages_consumed, 1u);

    // Clean shutdown — whole teardown must complete in < 10 s wall-clock.
    consumer.stop();
    pipeline->stop();
    g_server_running = false;
    if (server_thread.joinable()) server_thread.join();
}

#endif  // ZEPTO_OPCUA_AVAILABLE

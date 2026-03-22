// ============================================================================
// APEX-DB: Feed Handler Integration Example
// ============================================================================
// Feed Handler → APEX-DB Pipeline 통합 예제
// ============================================================================

#include "apex/core/pipeline.h"
#include "apex/feeds/fix_feed_handler.h"
#include "apex/feeds/multicast_receiver.h"
#include "apex/feeds/nasdaq_itch.h"
#include <iostream>
#include <unordered_map>
#include <string>
#include <csignal>
#include <atomic>

using namespace apex;

// ============================================================================
// Simple Symbol Mapper
// ============================================================================
class SimpleSymbolMapper : public feeds::SymbolMapper {
public:
    uint32_t get_symbol_id(const std::string& symbol) override {
        auto it = symbol_to_id_.find(symbol);
        if (it != symbol_to_id_.end()) {
            return it->second;
        }

        uint32_t id = next_id_++;
        symbol_to_id_[symbol] = id;
        id_to_symbol_[id] = symbol;
        return id;
    }

    std::string get_symbol_name(uint32_t symbol_id) override {
        auto it = id_to_symbol_.find(symbol_id);
        if (it != id_to_symbol_.end()) {
            return it->second;
        }
        return "UNKNOWN";
    }

private:
    std::unordered_map<std::string, uint32_t> symbol_to_id_;
    std::unordered_map<uint32_t, std::string> id_to_symbol_;
    uint32_t next_id_ = 1;
};

// ============================================================================
// Global shutdown flag
// ============================================================================
std::atomic<bool> g_shutdown{false};

void signal_handler(int signum) {
    std::cout << "\nShutdown signal received (" << signum << ")\n";
    g_shutdown.store(true);
}

// ============================================================================
// Example 1: FIX Feed → APEX-DB
// ============================================================================
void example_fix_feed() {
    std::cout << "=== Example 1: FIX Feed Handler ===\n";

    // APEX-DB Pipeline 초기화
    core::Pipeline pipeline;
    pipeline.start();

    // Symbol Mapper
    SimpleSymbolMapper mapper;

    // FIX Feed Handler 설정
    feeds::FeedConfig config;
    config.host = "localhost";  // FIX 서버 주소
    config.port = 5000;
    config.username = "APEX_CLIENT";
    config.password = "password";
    config.heartbeat_interval_ms = 30000;

    feeds::FIXFeedHandler feed_handler(config, &mapper);

    // Tick 콜백 등록 → APEX-DB로 인제스션
    feed_handler.on_tick([&](const feeds::Tick& tick) {
        pipeline.ingest(tick.symbol_id, tick.price, tick.volume, tick.timestamp_ns);

        // 통계 출력 (10000틱마다)
        static uint64_t count = 0;
        if (++count % 10000 == 0) {
            std::cout << "Ingested " << count << " ticks\n";
        }
    });

    // 에러 콜백
    feed_handler.on_error([](const std::string& error) {
        std::cerr << "Feed error: " << error << "\n";
    });

    // 연결 및 구독
    if (!feed_handler.connect()) {
        std::cerr << "Failed to connect to FIX server\n";
        return;
    }

    std::vector<std::string> symbols = {"AAPL", "MSFT", "TSLA"};
    if (!feed_handler.subscribe(symbols)) {
        std::cerr << "Failed to subscribe to symbols\n";
        return;
    }

    std::cout << "Connected and subscribed. Press Ctrl+C to stop.\n";

    // 메인 루프
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 통계 출력
        auto stats = pipeline.stats();
        std::cout << "Pipeline: "
                  << stats.ticks_ingested.load() << " ingested, "
                  << stats.ticks_stored.load() << " stored, "
                  << stats.ticks_dropped.load() << " dropped\n";
    }

    feed_handler.disconnect();
    pipeline.stop();
    std::cout << "Shutdown complete\n";
}

// ============================================================================
// Example 2: NASDAQ ITCH (Multicast UDP) → APEX-DB
// ============================================================================
void example_nasdaq_itch() {
    std::cout << "=== Example 2: NASDAQ ITCH Feed ===\n";

    // APEX-DB Pipeline 초기화
    core::Pipeline pipeline;
    pipeline.start();

    // Symbol Mapper
    SimpleSymbolMapper mapper;

    // NASDAQ ITCH Parser
    feeds::NASDAQITCHParser parser;

    // Multicast UDP Receiver
    // NASDAQ ITCH: 239.1.1.1:10000 (예시 주소)
    feeds::MulticastReceiver receiver("239.1.1.1", 10000);

    // 패킷 콜백: ITCH 파싱 → APEX-DB 인제스션
    receiver.on_packet([&](const uint8_t* data, size_t len) {
        if (!parser.parse_packet(data, len)) {
            return;
        }

        feeds::Tick tick;
        if (parser.extract_tick(tick, &mapper)) {
            pipeline.ingest(tick.symbol_id, tick.price, tick.volume, tick.timestamp_ns);
        }
    });

    // 멀티캐스트 그룹 가입
    if (!receiver.join()) {
        std::cerr << "Failed to join multicast group\n";
        return;
    }

    // 수신 시작
    receiver.start();
    std::cout << "Listening on 239.1.1.1:10000. Press Ctrl+C to stop.\n";

    // 메인 루프
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 통계 출력
        auto stats = pipeline.stats();
        std::cout << "Pipeline: "
                  << stats.ticks_ingested.load() << " ingested ("
                  << receiver.get_packet_count() << " packets, "
                  << receiver.get_byte_count() / 1024 / 1024 << " MB)\n";
    }

    receiver.stop();
    pipeline.stop();
    std::cout << "Shutdown complete\n";
}

// ============================================================================
// Example 3: Performance Test (Simulated Feed)
// ============================================================================
void example_performance_test() {
    std::cout << "=== Example 3: Performance Test ===\n";

    core::Pipeline pipeline;
    pipeline.start();

    SimpleSymbolMapper mapper;
    uint32_t symbol_id = mapper.get_symbol_id("TEST");

    std::cout << "Generating 10M ticks...\n";
    auto start = std::chrono::steady_clock::now();

    constexpr uint64_t NUM_TICKS = 10'000'000;
    for (uint64_t i = 0; i < NUM_TICKS; ++i) {
        double price = 100.0 + (i % 1000) * 0.01;
        uint64_t volume = 100 + (i % 500);
        uint64_t timestamp_ns = feeds::now_ns() + i;

        pipeline.ingest(symbol_id, price, volume, timestamp_ns);
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();

    std::cout << "Ingested " << NUM_TICKS << " ticks in " << elapsed << " seconds\n";
    std::cout << "Throughput: " << (NUM_TICKS / elapsed / 1'000'000.0) << " M ticks/sec\n";

    // 플러시 대기
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto stats = pipeline.stats();
    std::cout << "Final stats:\n";
    std::cout << "  Ingested: " << stats.ticks_ingested.load() << "\n";
    std::cout << "  Stored:   " << stats.ticks_stored.load() << "\n";
    std::cout << "  Dropped:  " << stats.ticks_dropped.load() << "\n";

    pipeline.stop();
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    // Signal handler 등록
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <example>\n";
        std::cout << "Examples:\n";
        std::cout << "  fix         - FIX Feed Handler\n";
        std::cout << "  itch        - NASDAQ ITCH (Multicast)\n";
        std::cout << "  perf        - Performance Test\n";
        return 1;
    }

    std::string example = argv[1];

    try {
        if (example == "fix") {
            example_fix_feed();
        } else if (example == "itch") {
            example_nasdaq_itch();
        } else if (example == "perf") {
            example_performance_test();
        } else {
            std::cerr << "Unknown example: " << example << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

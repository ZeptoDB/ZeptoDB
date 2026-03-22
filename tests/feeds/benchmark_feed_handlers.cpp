// ============================================================================
// APEX-DB: Feed Handler Benchmark Tests
// ============================================================================
#include "apex/feeds/fix_parser.h"
#include "apex/feeds/optimized/fix_parser_fast.h"
#include "apex/feeds/nasdaq_itch.h"
#include <benchmark/benchmark.h>
#include <cstring>

using namespace apex::feeds;

// ============================================================================
// Mock Symbol Mapper
// ============================================================================
class BenchmarkSymbolMapper : public SymbolMapper {
public:
    uint32_t get_symbol_id(const std::string& symbol) override {
        return 1;  // 항상 1 반환 (빠르게)
    }

    std::string get_symbol_name(uint32_t symbol_id) override {
        return "BENCH";
    }
};

// ============================================================================
// FIX Parser Benchmarks
// ============================================================================
static void BM_FIXParser_Parse(benchmark::State& state) {
    FIXParser parser;
    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=8\x01"
                      "55=AAPL\x01" "31=150.50\x01" "32=100\x01"
                      "54=1\x01" "10=123\x01";
    size_t len = std::strlen(msg);

    for (auto _ : state) {
        parser.parse(msg, len);
        benchmark::DoNotOptimize(parser);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("Standard Parser");
}
BENCHMARK(BM_FIXParser_Parse);

static void BM_FIXParserFast_Parse(benchmark::State& state) {
    optimized::FIXParserFast parser;
    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=8\x01"
                      "55=AAPL\x01" "31=150.50\x01" "32=100\x01"
                      "54=1\x01" "10=123\x01";
    size_t len = std::strlen(msg);

    for (auto _ : state) {
        parser.parse_zero_copy(msg, len);
        benchmark::DoNotOptimize(parser);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("Optimized Parser (Zero-Copy)");
}
BENCHMARK(BM_FIXParserFast_Parse);

static void BM_FIXParser_ExtractTick(benchmark::State& state) {
    FIXParser parser;
    BenchmarkSymbolMapper mapper;
    const char* msg = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                      "31=150.50\x01" "32=100\x01" "54=1\x01" "10=123\x01";

    parser.parse(msg, std::strlen(msg));

    for (auto _ : state) {
        Tick tick;
        parser.extract_tick(tick, &mapper);
        benchmark::DoNotOptimize(tick);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FIXParser_ExtractTick);

// ============================================================================
// ITCH Parser Benchmarks
// ============================================================================
static void BM_ITCHParser_Parse(benchmark::State& state) {
    NASDAQITCHParser parser;

    // Add Order 패킷 생성
    uint8_t packet[256];
    size_t offset = 0;

    auto write_uint16_be = [](uint8_t* buf, uint16_t value) {
        buf[0] = (value >> 8) & 0xFF;
        buf[1] = value & 0xFF;
    };

    auto write_uint32_be = [](uint8_t* buf, uint32_t value) {
        buf[0] = (value >> 24) & 0xFF;
        buf[1] = (value >> 16) & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = value & 0xFF;
    };

    auto write_uint64_be = [](uint8_t* buf, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            buf[i] = (value >> (56 - i * 8)) & 0xFF;
        }
    };

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';
    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);
    offset += 8;
    write_uint32_be(packet + offset, 1500000);
    offset += 4;

    size_t packet_len = offset;

    for (auto _ : state) {
        parser.parse_packet(packet, packet_len);
        benchmark::DoNotOptimize(parser);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ITCHParser_Parse);

static void BM_ITCHParser_ExtractTick(benchmark::State& state) {
    NASDAQITCHParser parser;
    BenchmarkSymbolMapper mapper;

    // 패킷 준비 (위와 동일)
    uint8_t packet[256];
    size_t offset = 0;

    auto write_uint16_be = [](uint8_t* buf, uint16_t value) {
        buf[0] = (value >> 8) & 0xFF;
        buf[1] = value & 0xFF;
    };

    auto write_uint32_be = [](uint8_t* buf, uint32_t value) {
        buf[0] = (value >> 24) & 0xFF;
        buf[1] = (value >> 16) & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = value & 0xFF;
    };

    auto write_uint64_be = [](uint8_t* buf, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            buf[i] = (value >> (56 - i * 8)) & 0xFF;
        }
    };

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';
    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);
    offset += 8;
    write_uint32_be(packet + offset, 1500000);
    offset += 4;

    parser.parse_packet(packet, offset);

    for (auto _ : state) {
        Tick tick;
        parser.extract_tick(tick, &mapper);
        benchmark::DoNotOptimize(tick);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ITCHParser_ExtractTick);

// ============================================================================
// Memory Pool Benchmarks
// ============================================================================
static void BM_TickAllocation_Malloc(benchmark::State& state) {
    for (auto _ : state) {
        Tick* tick = new Tick();
        benchmark::DoNotOptimize(tick);
        delete tick;
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("malloc/free");
}
BENCHMARK(BM_TickAllocation_Malloc);

static void BM_TickAllocation_MemoryPool(benchmark::State& state) {
    optimized::TickMemoryPool pool(100000);

    for (auto _ : state) {
        state.PauseTiming();
        pool.reset();
        state.ResumeTiming();

        Tick* tick = pool.allocate();
        benchmark::DoNotOptimize(tick);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetLabel("Memory Pool");
}
BENCHMARK(BM_TickAllocation_MemoryPool);

// ============================================================================
// Lock-free Ring Buffer Benchmarks
// ============================================================================
static void BM_RingBuffer_PushPop(benchmark::State& state) {
    optimized::LockFreeRingBuffer<Tick> buffer(10000);

    for (auto _ : state) {
        Tick tick;
        tick.symbol_id = 1;
        tick.price = 150.0;

        buffer.push(tick);

        Tick out_tick;
        buffer.pop(out_tick);

        benchmark::DoNotOptimize(out_tick);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_PushPop);

// ============================================================================
// End-to-End Benchmarks
// ============================================================================
static void BM_EndToEnd_FIX_to_Tick(benchmark::State& state) {
    FIXParser parser;
    BenchmarkSymbolMapper mapper;
    const char* msg = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                      "31=150.50\x01" "32=100\x01" "54=1\x01" "10=123\x01";
    size_t len = std::strlen(msg);

    for (auto _ : state) {
        parser.parse(msg, len);
        Tick tick;
        parser.extract_tick(tick, &mapper);
        benchmark::DoNotOptimize(tick);
    }

    double ns_per_msg = state.iterations() /
                        (state.elapsed_time() / benchmark::time_units::kNanosecond);
    state.SetLabel("FIX→Tick: " + std::to_string(ns_per_msg) + "ns");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEnd_FIX_to_Tick);

static void BM_EndToEnd_ITCH_to_Tick(benchmark::State& state) {
    NASDAQITCHParser parser;
    BenchmarkSymbolMapper mapper;

    // 패킷 준비
    uint8_t packet[256];
    size_t offset = 0;

    auto write_uint16_be = [](uint8_t* buf, uint16_t value) {
        buf[0] = (value >> 8) & 0xFF;
        buf[1] = value & 0xFF;
    };

    auto write_uint32_be = [](uint8_t* buf, uint32_t value) {
        buf[0] = (value >> 24) & 0xFF;
        buf[1] = (value >> 16) & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = value & 0xFF;
    };

    auto write_uint64_be = [](uint8_t* buf, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            buf[i] = (value >> (56 - i * 8)) & 0xFF;
        }
    };

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';
    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);
    offset += 8;
    write_uint32_be(packet + offset, 1500000);
    offset += 4;

    size_t packet_len = offset;

    for (auto _ : state) {
        parser.parse_packet(packet, packet_len);
        Tick tick;
        parser.extract_tick(tick, &mapper);
        benchmark::DoNotOptimize(tick);
    }

    double ns_per_msg = state.iterations() /
                        (state.elapsed_time() / benchmark::time_units::kNanosecond);
    state.SetLabel("ITCH→Tick: " + std::to_string(ns_per_msg) + "ns");
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EndToEnd_ITCH_to_Tick);

BENCHMARK_MAIN();

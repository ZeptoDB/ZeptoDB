// ============================================================================
// HDB Benchmark: 플러시 처리량, mmap 읽기 처리량, Tiered 쿼리 지연
// ============================================================================
// 측정 항목:
//   1. HDB 플러시 처리량 (MB/sec) — NVMe 쓰기 성능
//   2. mmap 읽기 처리량 (MB/sec) — DRAM/NVMe 읽기 성능
//   3. 쿼리 지연 비교 — Pure In-Memory vs Tiered (RDB + HDB) COUNT/VWAP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/hdb_reader.h"
#include "zeptodb/storage/flush_manager.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/common/logger.h"

namespace fs = std::filesystem;
using namespace zeptodb;
using namespace zeptodb::storage;
using namespace zeptodb::core;
using clock_t_ = std::chrono::high_resolution_clock;

// ============================================================================
// 유틸리티
// ============================================================================
static double elapsed_ms(const clock_t_::time_point& start) {
    return std::chrono::duration<double, std::milli>(
        clock_t_::now() - start).count();
}

static void print_header(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
}

static void print_result(const std::string& name, double value,
                          const std::string& unit) {
    std::cout << std::left << std::setw(40) << name
              << std::right << std::setw(12) << std::fixed
              << std::setprecision(2) << value
              << " " << unit << "\n";
}

// ============================================================================
// 테스트용 파티션 생성 헬퍼
// ============================================================================
static std::unique_ptr<Partition> make_bench_partition(
    SymbolId symbol, int64_t hour_epoch, size_t n_rows)
{
    auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
        .total_size = (n_rows * 64 + 4096 > 64ULL * 1024 * 1024)
                      ? n_rows * 64 + 4096
                      : 64ULL * 1024 * 1024,
        .use_hugepages = true,
    });

    PartitionKey key{0, symbol, hour_epoch};
    auto part = std::make_unique<Partition>(key, std::move(arena));

    part->add_column("timestamp",  ColumnType::TIMESTAMP_NS);
    part->add_column("price",      ColumnType::INT64);
    part->add_column("volume",     ColumnType::INT64);
    part->add_column("msg_type",   ColumnType::INT32);

    for (size_t i = 0; i < n_rows; ++i) {
        const int64_t ts = hour_epoch + static_cast<int64_t>(i) * 1'000'000LL;
        part->get_column("timestamp")->append<int64_t>(ts);
        part->get_column("price")->append<int64_t>(100'0000LL + static_cast<int64_t>(i % 10000));
        part->get_column("volume")->append<int64_t>(1000LL + static_cast<int64_t>(i % 500));
        part->get_column("msg_type")->append<int32_t>(1);
    }

    part->seal();
    return part;
}

// ============================================================================
// Bench 1: 플러시 처리량 (MB/sec)
// ============================================================================
static void bench_flush_throughput(const std::string& base_dir) {
    print_header("HDB Flush Throughput (NVMe Write)");

    const std::vector<size_t> row_counts = {
        100'000, 500'000, 1'000'000, 5'000'000
    };

    for (const size_t n_rows : row_counts) {
        const std::string bench_dir = base_dir + "/flush_bench";
        std::error_code ec;
        fs::remove_all(bench_dir, ec);
        fs::create_directories(bench_dir);

        HDBWriter writer(bench_dir, false);  // 압축 없이 순수 I/O 측정

        const SymbolId symbol    = 1;
        const int64_t  hour      = 3600LL * 1'000'000'000LL;
        auto part = make_bench_partition(symbol, hour, n_rows);

        const auto t0 = clock_t_::now();
        const size_t bytes = writer.flush_partition(*part);
        const double ms = elapsed_ms(t0);

        const double mb    = static_cast<double>(bytes) / (1024.0 * 1024.0);
        const double mbps  = mb / (ms / 1000.0);

        char label[64];
        std::snprintf(label, sizeof(label), "Flush %zuK rows (%.1f MB)",
                      n_rows / 1000, mb);
        print_result(label, mbps, "MB/sec");

        fs::remove_all(bench_dir, ec);
    }

    // LZ4 압축 처리량
    if (HDBWriter::lz4_available()) {
        const size_t n_rows = 1'000'000;
        const std::string bench_dir = base_dir + "/flush_lz4";
        std::error_code ec;
        fs::create_directories(bench_dir);

        HDBWriter writer(bench_dir, true);  // LZ4 ON

        const SymbolId symbol = 2;
        const int64_t  hour   = 3600LL * 1'000'000'000LL;
        auto part = make_bench_partition(symbol, hour, n_rows);

        const auto t0 = clock_t_::now();
        const size_t bytes = writer.flush_partition(*part);
        const double ms = elapsed_ms(t0);

        const double raw_size = static_cast<double>(n_rows) * 4 * 8;  // 4 cols x 8 bytes
        const double mb    = raw_size / (1024.0 * 1024.0);
        const double mbps  = mb / (ms / 1000.0);
        const double ratio = static_cast<double>(bytes) / raw_size;

        char label[64];
        std::snprintf(label, sizeof(label), "Flush LZ4 %zuK rows (ratio=%.2f)",
                      n_rows / 1000, ratio);
        print_result(label, mbps, "MB/sec (raw)");

        fs::remove_all(bench_dir, ec);
    }
}

// ============================================================================
// Bench 2: mmap 읽기 처리량 (MB/sec)
// ============================================================================
static void bench_read_throughput(const std::string& base_dir) {
    print_header("HDB mmap Read Throughput");

    const std::vector<size_t> row_counts = {
        100'000, 500'000, 1'000'000, 5'000'000
    };

    for (const size_t n_rows : row_counts) {
        const std::string bench_dir = base_dir + "/read_bench";
        std::error_code ec;
        fs::remove_all(bench_dir, ec);

        // 파일 준비
        {
            HDBWriter writer(bench_dir, false);
            const SymbolId symbol = 3;
            const int64_t  hour   = 3600LL * 1'000'000'000LL;
            auto part = make_bench_partition(symbol, hour, n_rows);
            writer.flush_partition(*part);
        }

        HDBReader reader(bench_dir);
        const SymbolId symbol = 3;
        const int64_t  hour   = 3600LL * 1'000'000'000LL;

        // 캐시 warming을 위해 1회 읽기
        {
            auto col = reader.read_column(symbol, hour, "price");
            (void)col;
        }

        // 4개 컬럼 순차 읽기 측정
        const auto t0 = clock_t_::now();
        size_t total_bytes = 0;
        volatile int64_t checksum = 0;  // 최적화 방지

        for (const std::string& col_name : {"timestamp", "price", "volume", "msg_type"}) {
            auto col = reader.read_column(symbol, hour, col_name);
            if (!col.valid()) continue;
            total_bytes += col.num_rows * column_type_size(col.type);

            // 실제 데이터 접근 (최적화 방지)
            if (col.type == ColumnType::INT64 || col.type == ColumnType::TIMESTAMP_NS) {
                const auto span = col.as_span<int64_t>();
                checksum ^= span[0] ^ span[span.size() - 1];
            }
        }

        const double ms   = elapsed_ms(t0);
        const double mb   = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
        const double mbps = mb / (ms / 1000.0);
        (void)checksum;

        char label[64];
        std::snprintf(label, sizeof(label), "Read mmap %zuK rows (%.1f MB)",
                      n_rows / 1000, mb);
        print_result(label, mbps, "MB/sec");

        fs::remove_all(bench_dir, ec);
    }
}

// ============================================================================
// Bench 3: 쿼리 지연 비교 — Pure In-Memory vs Tiered
// ============================================================================
static void bench_query_latency(const std::string& base_dir) {
    print_header("Query Latency: Pure In-Memory vs Tiered");

    const size_t   n_rows    = 1'000'000;
    const SymbolId symbol    = 10;
    const int64_t  ns_hour   = 3600LL * 1'000'000'000LL;
    const int       n_trials  = 5;

    // ===== Pure In-Memory 파이프라인 =====
    {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        cfg.arena_size_per_partition = 64ULL * 1024 * 1024;
        ZeptoPipeline pipeline(cfg);

        for (size_t i = 0; i < n_rows; ++i) {
            TickMessage msg{};
            msg.symbol_id = symbol;
            msg.recv_ts   = ns_hour + static_cast<int64_t>(i) * 1'000'000LL;
            msg.price     = 100'0000LL + static_cast<int64_t>(i % 1000);
            msg.volume    = 1000LL;
            msg.msg_type  = (uint8_t)0;
            pipeline.ingest_tick(msg);
        }
        pipeline.drain_sync();

        // COUNT 쿼리 지연
        std::vector<double> latencies;
        for (int t = 0; t < n_trials; ++t) {
            const auto r = pipeline.query_count(symbol, 0, INT64_MAX);
            latencies.push_back(static_cast<double>(r.latency_ns) / 1000.0);
        }
        const double avg_us = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                              / static_cast<double>(n_trials);
        print_result("Pure In-Memory COUNT 1M rows", avg_us, "µs avg");
    }

    // ===== Tiered 파이프라인 (HDB에서 데이터 읽기) =====
    {
        const std::string bench_dir = base_dir + "/tiered_bench";
        std::error_code ec;
        fs::remove_all(bench_dir, ec);

        // HDB에 1M 행 미리 기록
        {
            HDBWriter writer(bench_dir, false);
            auto part = make_bench_partition(symbol, ns_hour, n_rows);
            writer.flush_partition(*part);
        }

        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::TIERED;
        cfg.hdb_base_path = bench_dir;
        cfg.arena_size_per_partition = 4ULL * 1024 * 1024;
        ZeptoPipeline pipeline(cfg);

        // HDB COUNT 쿼리 지연
        std::vector<double> latencies;
        for (int t = 0; t < n_trials; ++t) {
            const auto r = pipeline.query_count(symbol, 0, INT64_MAX);
            latencies.push_back(static_cast<double>(r.latency_ns) / 1000.0);
        }
        const double avg_us = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                              / static_cast<double>(n_trials);
        print_result("Tiered (HDB mmap) COUNT 1M rows", avg_us, "µs avg");

        fs::remove_all(bench_dir, ec);
    }

    // ===== VWAP 쿼리 지연 비교 =====
    {
        PipelineConfig cfg;
        cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
        cfg.arena_size_per_partition = 64ULL * 1024 * 1024;
        ZeptoPipeline pipeline(cfg);

        const SymbolId sym2 = 20;
        for (size_t i = 0; i < n_rows; ++i) {
            TickMessage msg{};
            msg.symbol_id = sym2;
            msg.recv_ts   = ns_hour + static_cast<int64_t>(i) * 1'000'000LL;
            msg.price     = 100'0000LL + static_cast<int64_t>(i % 1000);
            msg.volume    = 1000LL + static_cast<int64_t>(i % 100);
            msg.msg_type  = (uint8_t)0;
            pipeline.ingest_tick(msg);
        }
        pipeline.drain_sync();

        std::vector<double> latencies;
        for (int t = 0; t < n_trials; ++t) {
            const auto r = pipeline.query_vwap(sym2, 0, INT64_MAX);
            latencies.push_back(static_cast<double>(r.latency_ns) / 1000.0);
        }
        const double avg_us = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                              / static_cast<double>(n_trials);
        print_result("Pure In-Memory VWAP 1M rows", avg_us, "µs avg");
    }
}

// ============================================================================
// main
// ============================================================================
int main() {
    Logger::init("bench_hdb", spdlog::level::warn);

    const std::string temp_dir = "/tmp/zepto_hdb_bench_" +
        std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count());
    fs::create_directories(temp_dir);

    std::cout << "==============================================\n";
    std::cout << "  ZeptoDB HDB Benchmarks\n";
    std::cout << "  LZ4 Available: " << (HDBWriter::lz4_available() ? "YES" : "NO") << "\n";
    std::cout << "==============================================\n";

    bench_flush_throughput(temp_dir);
    bench_read_throughput(temp_dir);
    bench_query_latency(temp_dir);

    std::cout << "\n[완료] 임시 파일 정리 중...\n";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    return 0;
}

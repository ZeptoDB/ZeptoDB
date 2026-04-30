// ============================================================================
// bench_ingest_scale — measure HTTP INSERT throughput across N pods
// ============================================================================
// Purpose-built for the multi-node scaling story. Key differences from
// bench_rebalance:
//   1. Persistent HTTP connections (httplib::Client reused per thread)
//   2. No rate limit — flood mode
//   3. Multi-row INSERT batching (--batch-size N)
//   4. Stats collected from ALL pods via headless service (not just LB)
//   5. Ensures CREATE TABLE on all pods before bench (DDL replication)
//
// Usage:
//   ./bench_ingest_scale --host <svc> --port 8123 \
//       --headless <headless-svc> --pods 3 --pod-prefix zepto-bench-zeptodb \
//       --symbols 80 --threads 8 --seconds 20 --batch-size 10
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#define CPPHTTPLIB_KEEPALIVE_MAX_COUNT 10000
#include "httplib.h"

using Clock = std::chrono::steady_clock;

struct Config {
    std::string host      = "zepto-bench-zeptodb.zeptodb-bench.svc.cluster.local";
    int         port      = 8123;
    std::string headless  = "zepto-bench-zeptodb-headless.zeptodb-bench.svc.cluster.local";
    std::string pod_prefix = "zepto-bench-zeptodb";
    int         pods      = 1;
    int         symbols   = 80;
    int         threads   = 8;
    int         seconds   = 20;
    int         batch     = 10;   // rows per INSERT
};

static Config parse_args(int argc, char* argv[]) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i+1<argc) ? argv[++i] : ""; };
        if (a == "--host")        c.host = next();
        else if (a == "--port")   c.port = std::stoi(next());
        else if (a == "--headless") c.headless = next();
        else if (a == "--pod-prefix") c.pod_prefix = next();
        else if (a == "--pods")   c.pods = std::stoi(next());
        else if (a == "--symbols") c.symbols = std::stoi(next());
        else if (a == "--threads") c.threads = std::stoi(next());
        else if (a == "--seconds") c.seconds = std::stoi(next());
        else if (a == "--batch-size") c.batch = std::stoi(next());
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: bench_ingest_scale [options]\n"
                      << "  --host HOST          LB service (default: zepto-bench-zeptodb...)\n"
                      << "  --port PORT          HTTP port (default: 8123)\n"
                      << "  --headless HOST      Headless service for per-pod stats\n"
                      << "  --pod-prefix PREFIX  Pod name prefix (default: zepto-bench-zeptodb)\n"
                      << "  --pods N             Number of pods (for stats collection)\n"
                      << "  --symbols N          Symbol count (default: 80)\n"
                      << "  --threads N          Parallel HTTP clients (default: 8)\n"
                      << "  --seconds N          Duration (default: 20)\n"
                      << "  --batch-size N       Rows per INSERT (default: 10)\n";
            std::exit(0);
        }
    }
    return c;
}

// Collect ticks_ingested from each pod via headless DNS
static int64_t sum_ticks_ingested(const Config& cfg) {
    int64_t total = 0;
    for (int p = 0; p < cfg.pods; ++p) {
        std::string pod_host = cfg.pod_prefix + "-" + std::to_string(p) +
                               "." + cfg.headless;
        httplib::Client cli(pod_host, cfg.port);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(3);
        auto res = cli.Get("/stats");
        if (res && res->status == 200) {
            // Parse ticks_ingested from JSON
            auto pos = res->body.find("\"ticks_ingested\":");
            if (pos != std::string::npos) {
                total += std::atoll(res->body.c_str() + pos + 17);
            }
        }
    }
    return total;
}

// Per-pod breakdown
static void print_per_pod(const Config& cfg) {
    for (int p = 0; p < cfg.pods; ++p) {
        std::string pod_host = cfg.pod_prefix + "-" + std::to_string(p) +
                               "." + cfg.headless;
        httplib::Client cli(pod_host, cfg.port);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(3);
        auto res = cli.Get("/stats");
        if (res && res->status == 200) {
            printf("  pod-%d: %s\n", p, res->body.c_str());
        } else {
            printf("  pod-%d: (unreachable)\n", p);
        }
    }
}

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

    printf("bench_ingest_scale: host=%s:%d pods=%d threads=%d batch=%d symbols=%d seconds=%d\n",
           cfg.host.c_str(), cfg.port, cfg.pods, cfg.threads, cfg.batch,
           cfg.symbols, cfg.seconds);

    // Ensure table exists (DDL replication will propagate to all pods)
    {
        httplib::Client cli(cfg.host, cfg.port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        cli.Post("/", "DROP TABLE IF EXISTS trades", "text/plain");
        cli.Post("/", "CREATE TABLE IF NOT EXISTS trades (symbol INT64, timestamp TIMESTAMP_NS, price INT64, volume INT64)", "text/plain");
        std::this_thread::sleep_for(std::chrono::seconds(2));  // let DDL replicate
    }

    // Capture baseline
    int64_t before = sum_ticks_ingested(cfg);
    printf("before: %lld total ticks_ingested across %d pods\n",
           (long long)before, cfg.pods);

    // Flood
    std::atomic<bool> running{true};
    std::atomic<int64_t> total_ok{0}, total_fail{0};

    auto worker = [&](int id) {
        httplib::Client cli(cfg.host, cfg.port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        cli.set_keep_alive(true);

        std::mt19937 rng(id * 12345 + 67890);
        std::uniform_int_distribution<int> sym_dist(0, cfg.symbols - 1);
        int64_t ok = 0, fail = 0;

        while (running.load(std::memory_order_relaxed)) {
            // Build multi-row INSERT
            std::string sql = "INSERT INTO trades VALUES ";
            for (int r = 0; r < cfg.batch; ++r) {
                if (r > 0) sql += ", ";
                int sym = sym_dist(rng);
                auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                sql += "(" + std::to_string(sym) + ", " + std::to_string(ts) +
                       ", " + std::to_string(15000 + sym) + ", 100)";
            }

            auto res = cli.Post("/", sql, "text/plain");
            if (res && res->status == 200)
                ok += cfg.batch;
            else
                fail += cfg.batch;
        }
        total_ok.fetch_add(ok, std::memory_order_relaxed);
        total_fail.fetch_add(fail, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; ++t)
        threads.emplace_back(worker, t);

    std::this_thread::sleep_for(std::chrono::seconds(cfg.seconds));
    running.store(false, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    // Collect after
    std::this_thread::sleep_for(std::chrono::seconds(1));  // let drain catch up
    int64_t after = sum_ticks_ingested(cfg);
    int64_t delta = after - before;
    double rate = static_cast<double>(delta) / cfg.seconds;

    printf("\n=== RESULTS ===\n");
    printf("duration:    %d s\n", cfg.seconds);
    printf("threads:     %d\n", cfg.threads);
    printf("batch_size:  %d\n", cfg.batch);
    printf("client_ok:   %lld ticks\n", (long long)total_ok.load());
    printf("client_fail: %lld ticks\n", (long long)total_fail.load());
    printf("server_delta:%lld ticks (sum across %d pods)\n", (long long)delta, cfg.pods);
    printf("RATE:        %.0f ticks/sec\n", rate);
    printf("\nPer-pod breakdown:\n");
    print_per_pod(cfg);

    return 0;
}

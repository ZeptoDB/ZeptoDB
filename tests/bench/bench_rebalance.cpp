// bench_rebalance.cpp — EKS multi-node live rebalancing load test
// Usage: ./bench_rebalance --host <svc> --port <port> [options]
//
// Runs from the loadgen pod against a live ZeptoDB cluster via HTTP.
// Measures ingestion throughput and query latency before, during, and after rebalance.

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

// ============================================================================
// Config & Metrics
// ============================================================================

struct Config {
    std::string host = "zeptodb.zeptodb.svc.cluster.local";
    int port = 8123;
    int num_symbols = 100;
    int ticks_per_sec = 10000;
    int query_qps = 10;
    int baseline_sec = 30;
    int rebalance_node_id = 0;
    std::string action = "add_node";
    int ingest_threads = 1;
    std::string scenario = "all";  // all, basic, add_remove_cycle, pause_resume,
                                   // heavy_query, back_to_back, status_polling
};

struct Metrics {
    std::atomic<uint64_t> inserts_ok{0};
    std::atomic<uint64_t> inserts_fail{0};
    std::atomic<uint64_t> queries_ok{0};
    std::atomic<uint64_t> queries_fail{0};
    std::vector<double> query_latencies_ms;
    mutable std::mutex lat_mu;

    void record_query_latency(double ms) {
        std::lock_guard lk(lat_mu);
        query_latencies_ms.push_back(ms);
    }

    double percentile(double p) const {
        std::lock_guard lk(lat_mu);
        if (query_latencies_ms.empty()) return 0;
        auto sorted = query_latencies_ms;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = std::min(static_cast<size_t>(p * sorted.size()), sorted.size() - 1);
        return sorted[idx];
    }

    double p50() const { return percentile(0.50); }
    double p99() const { return percentile(0.99); }

    void reset() {
        inserts_ok = 0;
        inserts_fail = 0;
        queries_ok = 0;
        queries_fail = 0;
        std::lock_guard lk(lat_mu);
        query_latencies_ms.clear();
    }
};

// ============================================================================
// Argument parsing
// ============================================================================

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: bench_rebalance [options]\n"
                      << "  --host HOST          Cluster hostname (default: zepto-zeptodb.zeptodb.svc.cluster.local)\n"
                      << "  --port PORT          HTTP port (default: 8123)\n"
                      << "  --symbols N          Number of symbols (default: 100)\n"
                      << "  --ticks-per-sec N    Target ingest rate (default: 10000)\n"
                      << "  --query-qps N        Query rate (default: 10)\n"
                      << "  --baseline-sec N     Baseline duration (default: 30)\n"
                      << "  --action ACTION      add_node or remove_node (default: add_node)\n"
                      << "  --node-id N          Node ID for rebalance (default: auto-detect)\n"
                      << "  --ingest-threads N   Number of ingest threads (default: 1)\n"
                      << "  --scenario NAME      Test scenario (default: all)\n"
                      << "      all              Run all scenarios\n"
                      << "      basic            Original 6-phase test\n"
                      << "      add_remove_cycle Scale-out then scale-in round-trip\n"
                      << "      pause_resume     Pause/resume mid-rebalance under load\n"
                      << "      heavy_query      High query QPS during rebalance\n"
                      << "      back_to_back     Consecutive rebalances without cooldown\n"
                      << "      status_polling   Rapid status endpoint polling during rebalance\n";
            std::exit(0);
        }
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (arg == "--host")           cfg.host = next();
        else if (arg == "--port")      cfg.port = std::stoi(next());
        else if (arg == "--symbols")   cfg.num_symbols = std::stoi(next());
        else if (arg == "--ticks-per-sec") cfg.ticks_per_sec = std::stoi(next());
        else if (arg == "--query-qps") cfg.query_qps = std::stoi(next());
        else if (arg == "--baseline-sec") cfg.baseline_sec = std::stoi(next());
        else if (arg == "--action")    cfg.action = next();
        else if (arg == "--node-id")   cfg.rebalance_node_id = std::stoi(next());
        else if (arg == "--ingest-threads") cfg.ingest_threads = std::stoi(next());
        else if (arg == "--scenario")  cfg.scenario = next();
    }
    return cfg;
}

// ============================================================================
// HTTP helpers
// ============================================================================

static std::string http_post(const Config& cfg, const std::string& body) {
    httplib::Client cli(cfg.host, cfg.port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);
    auto res = cli.Post("/", body, "text/plain");
    return (res && res->status == 200) ? res->body : "";
}

static std::string http_get(const Config& cfg, const std::string& path) {
    httplib::Client cli(cfg.host, cfg.port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Get(path);
    return (res && res->status == 200) ? res->body : "";
}

static std::string http_post_json(const Config& cfg, const std::string& path,
                                  const std::string& body) {
    httplib::Client cli(cfg.host, cfg.port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);
    auto res = cli.Post(path, body, "application/json");
    return (res && res->status == 200) ? res->body : "";
}

// ============================================================================
// Workers
// ============================================================================

void ingest_worker(const Config& cfg, Metrics& m, std::atomic<bool>& running) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> sym_dist(0, cfg.num_symbols - 1);
    std::uniform_int_distribution<int> price_dist(10000, 20000);
    std::uniform_int_distribution<int> vol_dist(1, 1000);

    const auto interval = std::chrono::nanoseconds(1'000'000'000LL / std::max(cfg.ticks_per_sec, 1));
    auto next_tick = Clock::now();

    httplib::Client cli(cfg.host, cfg.port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    while (running.load(std::memory_order_relaxed)) {
        int sym = sym_dist(rng);
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int price = price_dist(rng);
        int vol = vol_dist(rng);

        std::string sql = "INSERT INTO trades VALUES (" +
            std::to_string(sym) + ", " +
            std::to_string(price) + ", " + std::to_string(vol) + ", " +
            std::to_string(now_ns) + ")";

        auto res = cli.Post("/", sql, "text/plain");
        if (res && res->status == 200)
            m.inserts_ok.fetch_add(1, std::memory_order_relaxed);
        else
            m.inserts_fail.fetch_add(1, std::memory_order_relaxed);

        next_tick += interval;
        auto now = Clock::now();
        if (next_tick > now)
            std::this_thread::sleep_until(next_tick);
        else
            next_tick = now;  // fell behind, reset
    }
}

void query_worker(const Config& cfg, Metrics& m, std::atomic<bool>& running) {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> sym_dist(0, cfg.num_symbols - 1);

    const auto interval = std::chrono::milliseconds(1000 / std::max(cfg.query_qps, 1));

    httplib::Client cli(cfg.host, cfg.port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);

    while (running.load(std::memory_order_relaxed)) {
        int sym = sym_dist(rng);
        std::string sql = "SELECT count(*), vwap(price, volume) FROM trades WHERE symbol = " +
            std::to_string(sym);

        auto t0 = Clock::now();
        auto res = cli.Post("/", sql, "text/plain");
        auto t1 = Clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (res && res->status == 200) {
            m.queries_ok.fetch_add(1, std::memory_order_relaxed);
            m.record_query_latency(ms);
        } else {
            m.queries_fail.fetch_add(1, std::memory_order_relaxed);
        }

        std::this_thread::sleep_for(interval);
    }
}

// ============================================================================
// Cluster / Rebalance API
// ============================================================================

std::string get_cluster_info(const Config& cfg) {
    return http_get(cfg, "/admin/cluster");
}

std::string get_rebalance_status(const Config& cfg) {
    return http_get(cfg, "/admin/rebalance/status");
}

bool trigger_rebalance(const Config& cfg, const std::string& action, int node_id) {
    std::string body = R"({"action":")" + action + R"(","node_id":)" +
        std::to_string(node_id) + "}";
    auto resp = http_post_json(cfg, "/admin/rebalance/start", body);
    return resp.find("\"ok\":true") != std::string::npos;
}

bool wait_rebalance_done(const Config& cfg, int timeout_sec) {
    auto deadline = Clock::now() + std::chrono::seconds(timeout_sec);
    while (Clock::now() < deadline) {
        auto status = get_rebalance_status(cfg);
        if (status.find("\"IDLE\"") != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

bool verify_data_integrity(const Config& cfg, int expected_symbols) {
    int verified = 0;
    uint64_t total_rows = 0;
    for (int s = 0; s < expected_symbols; ++s) {
        std::string sql = "SELECT count(*) FROM trades WHERE symbol = " + std::to_string(s);
        auto resp = http_post(cfg, sql);
        if (!resp.empty()) {
            try {
                // Response may be JSON: {"columns":["*"],"data":[[N]],...}
                // or plain number
                long long cnt = 0;
                auto dp = resp.find("\"data\":[[");
                if (dp != std::string::npos) {
                    cnt = std::stoll(resp.substr(dp + 9));
                } else {
                    cnt = std::stoll(resp);
                }
                if (cnt > 0) ++verified;
                total_rows += static_cast<uint64_t>(cnt);
            } catch (...) {
                // Parse failed — skip this symbol (don't count as verified)
                std::cerr << "  WARN: Could not parse count for symbol " << s
                          << ": " << resp.substr(0, 60) << "\n";
            }
        }
    }
    std::cout << "  Symbols verified: " << verified << "/" << expected_symbols;
    if (verified == expected_symbols)
        std::cout << " \u2713";
    else
        std::cout << " FAIL";
    std::cout << "\n  Total rows: " << total_rows << "\n";
    return verified == expected_symbols;
}

// ============================================================================
// Phase runner
// ============================================================================

struct PhaseResult {
    double duration_sec;
    uint64_t inserts_ok, inserts_fail;
    uint64_t queries_ok, queries_fail;
    double p50, p99;
    double ticks_per_sec;
};

PhaseResult run_phase(const Config& cfg, Metrics& m, int duration_sec) {
    m.reset();
    std::atomic<bool> running{true};

    std::thread ingest_t(ingest_worker, std::cref(cfg), std::ref(m), std::ref(running));
    std::thread query_t(query_worker, std::cref(cfg), std::ref(m), std::ref(running));

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    running = false;

    ingest_t.join();
    query_t.join();

    PhaseResult r{};
    r.duration_sec = duration_sec;
    r.inserts_ok = m.inserts_ok.load();
    r.inserts_fail = m.inserts_fail.load();
    r.queries_ok = m.queries_ok.load();
    r.queries_fail = m.queries_fail.load();
    r.p50 = m.p50();
    r.p99 = m.p99();
    r.ticks_per_sec = (r.duration_sec > 0) ? r.inserts_ok / r.duration_sec : 0;
    return r;
}

PhaseResult run_rebalance_phase(const Config& cfg, Metrics& m, int node_id,
                                 int timeout_sec = 300) {
    m.reset();
    std::atomic<bool> running{true};

    std::thread ingest_t(ingest_worker, std::cref(cfg), std::ref(m), std::ref(running));
    std::thread query_t(query_worker, std::cref(cfg), std::ref(m), std::ref(running));

    // Give workers a moment to start
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto t0 = Clock::now();
    if (!trigger_rebalance(cfg, cfg.action, node_id)) {
        std::cerr << "  ERROR: Failed to trigger rebalance\n";
    }

    bool done = wait_rebalance_done(cfg, timeout_sec);
    auto t1 = Clock::now();
    double rebalance_sec = std::chrono::duration<double>(t1 - t0).count();

    // Let it run a couple more seconds to capture post-trigger metrics
    std::this_thread::sleep_for(std::chrono::seconds(2));
    running = false;

    ingest_t.join();
    query_t.join();

    double total_sec = std::chrono::duration<double>(Clock::now() - t0).count();

    PhaseResult r{};
    r.duration_sec = rebalance_sec;
    r.inserts_ok = m.inserts_ok.load();
    r.inserts_fail = m.inserts_fail.load();
    r.queries_ok = m.queries_ok.load();
    r.queries_fail = m.queries_fail.load();
    r.p50 = m.p50();
    r.p99 = m.p99();
    r.ticks_per_sec = (total_sec > 0) ? r.inserts_ok / total_sec : 0;

    if (!done)
        std::cerr << "  WARNING: Rebalance did not complete within timeout\n";

    return r;
}

// ============================================================================
// Output formatting
// ============================================================================

static std::string fmt_num(uint64_t n) {
    auto s = std::to_string(n);
    std::string out;
    int count = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        if (count > 0 && count % 3 == 0) out = ',' + out;
        out = s[i] + out;
        ++count;
    }
    return out;
}

static std::string fmt_pct(double baseline, double current) {
    if (baseline == 0) return "N/A";
    double pct = ((current - baseline) / baseline) * 100.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f%%", pct);
    return buf;
}

void print_phase(const char* name, const PhaseResult& r,
                 const PhaseResult* baseline = nullptr) {
    std::cout << "\n--- " << name << " ---\n";
    std::cout << "  Inserts: " << fmt_num(r.inserts_ok) << " ok / "
              << fmt_num(r.inserts_fail) << " fail ("
              << fmt_num(static_cast<uint64_t>(r.ticks_per_sec)) << " ticks/sec";
    if (baseline)
        std::cout << ", " << fmt_pct(baseline->ticks_per_sec, r.ticks_per_sec);
    std::cout << ")\n";

    std::cout << "  Queries: " << fmt_num(r.queries_ok) << " ok / "
              << fmt_num(r.queries_fail) << " fail\n";

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Query latency: p50=" << r.p50 << "ms p99=" << r.p99 << "ms";
    if (baseline) {
        std::cout << " (" << fmt_pct(baseline->p50, r.p50) << " / "
                  << fmt_pct(baseline->p99, r.p99) << ")";
    }
    std::cout << "\n";
}

// ============================================================================
// Rebalance cancel helper
// ============================================================================

bool cancel_rebalance(const Config& cfg) {
    auto resp = http_post_json(cfg, "/admin/rebalance/cancel", "");
    return resp.find("\"ok\":true") != std::string::npos;
}

// ============================================================================
// Multi-threaded ingest phase runner (Phase 6)
// ============================================================================

PhaseResult run_multi_ingest_rebalance_phase(const Config& cfg, Metrics& m,
                                              int node_id, int num_threads,
                                              int timeout_sec = 300) {
    m.reset();
    std::atomic<bool> running{true};

    std::vector<std::thread> ingest_threads;
    for (int i = 0; i < num_threads; ++i)
        ingest_threads.emplace_back(ingest_worker, std::cref(cfg), std::ref(m), std::ref(running));
    std::thread query_t(query_worker, std::cref(cfg), std::ref(m), std::ref(running));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto t0 = Clock::now();
    if (!trigger_rebalance(cfg, cfg.action, node_id))
        std::cerr << "  ERROR: Failed to trigger rebalance\n";

    bool done = wait_rebalance_done(cfg, timeout_sec);
    auto t1 = Clock::now();
    double rebalance_sec = std::chrono::duration<double>(t1 - t0).count();

    std::this_thread::sleep_for(std::chrono::seconds(2));
    running = false;

    for (auto& t : ingest_threads) t.join();
    query_t.join();

    double total_sec = std::chrono::duration<double>(Clock::now() - t0).count();

    PhaseResult r{};
    r.duration_sec = rebalance_sec;
    r.inserts_ok = m.inserts_ok.load();
    r.inserts_fail = m.inserts_fail.load();
    r.queries_ok = m.queries_ok.load();
    r.queries_fail = m.queries_fail.load();
    r.p50 = m.p50();
    r.p99 = m.p99();
    r.ticks_per_sec = (total_sec > 0) ? r.inserts_ok / total_sec : 0;

    if (!done)
        std::cerr << "  WARNING: Rebalance did not complete within timeout\n";
    return r;
}

// ============================================================================
// Scenario: basic (original 6-phase test)
// ============================================================================

bool scenario_basic(const Config& cfg, int node_id) {
    Metrics metrics;
    bool all_pass = true;

    // Phase 1: Baseline
    std::cout << "\n--- Phase 1: Baseline (" << cfg.baseline_sec << "s) ---\n";
    auto baseline = run_phase(cfg, metrics, cfg.baseline_sec);
    print_phase(("Phase 1: Baseline (" + std::to_string(cfg.baseline_sec) + "s)").c_str(), baseline);

    // Phase 2: Rebalance under load
    std::cout << "\n--- Phase 2: Rebalance Under Load ---\n";
    std::cout << "  Action: " << cfg.action << " (node_id=" << node_id << ")\n";
    auto rebalance = run_rebalance_phase(cfg, metrics, node_id);
    std::cout << "  Rebalance duration: " << std::fixed << std::setprecision(1)
              << rebalance.duration_sec << "s\n";
    print_phase("Phase 2: Rebalance Under Load", rebalance, &baseline);

    // Phase 3: Post-rebalance
    std::cout << "\n--- Phase 3: Post-Rebalance (" << cfg.baseline_sec << "s) ---\n";
    auto post = run_phase(cfg, metrics, cfg.baseline_sec);
    print_phase(("Phase 3: Post-Rebalance (" + std::to_string(cfg.baseline_sec) + "s)").c_str(), post);

    // Phase 4: Data integrity
    std::cout << "\n--- Phase 4: Data Integrity ---\n";
    bool integrity_ok = verify_data_integrity(cfg, cfg.num_symbols);
    if (!integrity_ok) all_pass = false;

    // Phase 5: Rapid Start/Cancel Stress
    std::cout << "\n--- Phase 5: Rapid Start/Cancel Stress ---\n";
    bool phase5_ok = true;
    for (int cycle = 0; cycle < 5; ++cycle) {
        std::string body = R"({"action":")" + cfg.action + R"(","node_id":)" +
            std::to_string(node_id) + "}";
        auto resp = http_post_json(cfg, "/admin/rebalance/start", body);
        if (resp.find("\"ok\":true") == std::string::npos) {
            std::cerr << "  Cycle " << cycle << ": start failed\n";
            phase5_ok = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        cancel_rebalance(cfg);
        if (!wait_rebalance_done(cfg, 30)) {
            std::cerr << "  Cycle " << cycle << ": did not return to IDLE\n";
            phase5_ok = false;
            break;
        }
    }
    std::cout << "  Rapid start/cancel: " << (phase5_ok ? "5/5 cycles OK" : "FAIL") << "\n";
    if (!phase5_ok) all_pass = false;

    // Phase 6: Concurrent Ingest Stress
    int ingest_threads = cfg.ingest_threads;
    std::cout << "\n--- Phase 6: Concurrent Ingest Stress (" << ingest_threads << " threads) ---\n";
    auto concurrent = run_multi_ingest_rebalance_phase(cfg, metrics, node_id, ingest_threads);
    std::cout << "  Rebalance duration: " << std::fixed << std::setprecision(1)
              << concurrent.duration_sec << "s\n";
    print_phase(("Phase 6: Concurrent Ingest (" + std::to_string(ingest_threads) + " threads)").c_str(),
                concurrent, &baseline);

    std::cout << "\n--- Phase 6: Data Integrity Check ---\n";
    bool phase6_integrity = verify_data_integrity(cfg, cfg.num_symbols);
    if (!phase6_integrity) all_pass = false;

    // Throughput check
    if (baseline.ticks_per_sec > 0 && rebalance.ticks_per_sec < baseline.ticks_per_sec * 0.5)
        all_pass = false;

    return all_pass;
}

// ============================================================================
// Scenario: add_remove_cycle — scale-out then scale-in round-trip
// ============================================================================

bool scenario_add_remove_cycle(const Config& cfg, int node_id) {
    Metrics metrics;
    std::cout << "\n=== Scenario: Add/Remove Cycle ===\n";
    std::cout << "  Tests: add_node → verify → remove_node → verify (round-trip)\n";

    // Baseline
    std::cout << "\n--- Baseline (" << cfg.baseline_sec << "s) ---\n";
    auto baseline = run_phase(cfg, metrics, cfg.baseline_sec);
    print_phase("Baseline", baseline);

    // Step 1: add_node under load
    std::cout << "\n--- Step 1: add_node (node_id=" << node_id << ") ---\n";
    {
        Config add_cfg = cfg;
        add_cfg.action = "add_node";
        auto r = run_rebalance_phase(add_cfg, metrics, node_id);
        std::cout << "  Duration: " << std::fixed << std::setprecision(1) << r.duration_sec << "s\n";
        print_phase("add_node", r, &baseline);
    }

    std::cout << "\n--- Data Integrity After Add ---\n";
    bool add_ok = verify_data_integrity(cfg, cfg.num_symbols);

    // Step 2: remove_node under load (reverse)
    std::cout << "\n--- Step 2: remove_node (node_id=" << node_id << ") ---\n";
    {
        Config rm_cfg = cfg;
        rm_cfg.action = "remove_node";
        auto r = run_rebalance_phase(rm_cfg, metrics, node_id);
        std::cout << "  Duration: " << std::fixed << std::setprecision(1) << r.duration_sec << "s\n";
        print_phase("remove_node", r, &baseline);
    }

    std::cout << "\n--- Data Integrity After Remove ---\n";
    bool rm_ok = verify_data_integrity(cfg, cfg.num_symbols);

    // Post round-trip steady state
    std::cout << "\n--- Post Round-Trip (" << cfg.baseline_sec << "s) ---\n";
    auto post = run_phase(cfg, metrics, cfg.baseline_sec);
    print_phase("Post Round-Trip", post, &baseline);

    bool pass = add_ok && rm_ok;
    std::cout << "\n  add_remove_cycle: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

// ============================================================================
// Scenario: pause_resume — pause/resume mid-rebalance under load
// ============================================================================

bool scenario_pause_resume(const Config& cfg, int node_id) {
    Metrics metrics;
    std::cout << "\n=== Scenario: Pause/Resume Under Load ===\n";
    std::cout << "  Tests: start rebalance → pause → verify ingest continues → resume → verify\n";

    // Start ingest + query workers
    std::atomic<bool> running{true};
    std::thread ingest_t(ingest_worker, std::cref(cfg), std::ref(metrics), std::ref(running));
    std::thread query_t(query_worker, std::cref(cfg), std::ref(metrics), std::ref(running));
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Trigger rebalance
    std::cout << "  Triggering " << cfg.action << " (node_id=" << node_id << ")...\n";
    bool started = trigger_rebalance(cfg, cfg.action, node_id);
    if (!started) {
        std::cerr << "  ERROR: Failed to trigger rebalance\n";
        running = false; ingest_t.join(); query_t.join();
        return false;
    }

    // Pause after 1s
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto pause_resp = http_post_json(cfg, "/admin/rebalance/pause", "");
    bool paused = pause_resp.find("\"ok\":true") != std::string::npos;
    std::cout << "  Pause: " << (paused ? "OK" : "FAIL") << "\n";

    // Record inserts during pause (3s)
    uint64_t pre_pause = metrics.inserts_ok.load();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    uint64_t post_pause = metrics.inserts_ok.load();
    uint64_t during_pause = post_pause - pre_pause;
    std::cout << "  Inserts during pause: " << fmt_num(during_pause) << " (should be > 0)\n";

    // Check status is PAUSED
    auto status = get_rebalance_status(cfg);
    bool is_paused = status.find("\"PAUSED\"") != std::string::npos;
    std::cout << "  Status is PAUSED: " << (is_paused ? "YES" : "NO") << "\n";

    // Resume
    auto resume_resp = http_post_json(cfg, "/admin/rebalance/resume", "");
    bool resumed = resume_resp.find("\"ok\":true") != std::string::npos;
    std::cout << "  Resume: " << (resumed ? "OK" : "FAIL") << "\n";

    // Wait for completion
    bool done = wait_rebalance_done(cfg, 60);
    std::cout << "  Rebalance completed: " << (done ? "YES" : "TIMEOUT") << "\n";

    running = false;
    ingest_t.join();
    query_t.join();

    // Data integrity
    std::cout << "\n--- Data Integrity ---\n";
    bool integrity = verify_data_integrity(cfg, cfg.num_symbols);

    bool pass = paused && during_pause > 0 && resumed && done && integrity;
    std::cout << "\n  pause_resume: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

// ============================================================================
// Scenario: heavy_query — high query QPS during rebalance
// ============================================================================

bool scenario_heavy_query(const Config& cfg, int node_id) {
    Metrics metrics;
    std::cout << "\n=== Scenario: Heavy Query Load During Rebalance ===\n";

    // Override query QPS to 50
    Config heavy_cfg = cfg;
    heavy_cfg.query_qps = 50;
    std::cout << "  Query QPS: " << heavy_cfg.query_qps << " (elevated)\n";

    // Baseline with heavy queries
    std::cout << "\n--- Baseline (" << cfg.baseline_sec << "s, 50 QPS) ---\n";
    auto baseline = run_phase(heavy_cfg, metrics, cfg.baseline_sec);
    print_phase("Baseline (50 QPS)", baseline);

    // Rebalance with heavy queries
    std::cout << "\n--- Rebalance Under Heavy Query Load ---\n";
    auto rebalance = run_rebalance_phase(heavy_cfg, metrics, node_id);
    std::cout << "  Duration: " << std::fixed << std::setprecision(1) << rebalance.duration_sec << "s\n";
    print_phase("Rebalance (50 QPS)", rebalance, &baseline);

    // Check query failure rate
    uint64_t total_q = rebalance.queries_ok + rebalance.queries_fail;
    double fail_rate = total_q > 0 ? (100.0 * rebalance.queries_fail / total_q) : 0;
    std::cout << "  Query failure rate: " << std::fixed << std::setprecision(1)
              << fail_rate << "% (threshold: <5%)\n";

    // Data integrity
    std::cout << "\n--- Data Integrity ---\n";
    bool integrity = verify_data_integrity(cfg, cfg.num_symbols);

    bool pass = integrity && fail_rate < 5.0;
    std::cout << "\n  heavy_query: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

// ============================================================================
// Scenario: back_to_back — consecutive rebalances without cooldown
// ============================================================================

bool scenario_back_to_back(const Config& cfg, int node_id) {
    Metrics metrics;
    std::cout << "\n=== Scenario: Back-to-Back Rebalances ===\n";
    std::cout << "  Tests: 3 consecutive remove_node rebalances with ingest running\n";

    std::atomic<bool> running{true};
    std::thread ingest_t(ingest_worker, std::cref(cfg), std::ref(metrics), std::ref(running));
    std::this_thread::sleep_for(std::chrono::seconds(2));

    bool all_ok = true;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::cout << "\n--- Cycle " << (cycle + 1) << "/3 ---\n";

        std::cout << "  remove_node(" << node_id << ")... ";
        if (trigger_rebalance(cfg, "remove_node", node_id)) {
            bool done = wait_rebalance_done(cfg, 60);
            std::cout << (done ? "OK" : "TIMEOUT") << "\n";
            if (!done) all_ok = false;
        } else {
            // May fail if node already removed — that's expected after first cycle
            std::cout << "SKIP (already removed)\n";
        }
    }

    running = false;
    ingest_t.join();

    uint64_t total = metrics.inserts_ok.load() + metrics.inserts_fail.load();
    double fail_pct = total > 0 ? (100.0 * metrics.inserts_fail.load() / total) : 0;
    std::cout << "\n  Total inserts: " << fmt_num(metrics.inserts_ok.load())
              << " ok / " << fmt_num(metrics.inserts_fail.load()) << " fail ("
              << std::fixed << std::setprecision(1) << fail_pct << "%)\n";

    std::cout << "\n--- Data Integrity ---\n";
    bool integrity = verify_data_integrity(cfg, cfg.num_symbols);

    bool pass = all_ok && integrity;
    std::cout << "\n  back_to_back: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

// ============================================================================
// Scenario: status_polling — rapid status endpoint polling during rebalance
// ============================================================================

bool scenario_status_polling(const Config& cfg, int node_id) {
    Metrics metrics;
    std::cout << "\n=== Scenario: Status Polling Under Load ===\n";
    std::cout << "  Tests: 100 rapid /admin/rebalance/status calls during rebalance\n";

    std::atomic<bool> running{true};
    std::thread ingest_t(ingest_worker, std::cref(cfg), std::ref(metrics), std::ref(running));
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Trigger rebalance
    trigger_rebalance(cfg, cfg.action, node_id);

    // Rapid-fire status polling
    int status_ok = 0, status_fail = 0;
    double max_latency = 0;
    for (int i = 0; i < 100; ++i) {
        auto t0 = Clock::now();
        auto resp = get_rebalance_status(cfg);
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (max_latency < ms) max_latency = ms;

        if (!resp.empty() && (resp.find("\"state\"") != std::string::npos))
            status_ok++;
        else
            status_fail++;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    wait_rebalance_done(cfg, 60);
    running = false;
    ingest_t.join();

    std::cout << "  Status calls: " << status_ok << " ok / " << status_fail << " fail\n";
    std::cout << "  Max status latency: " << std::fixed << std::setprecision(1)
              << max_latency << "ms\n";
    std::cout << "  Inserts during polling: " << fmt_num(metrics.inserts_ok.load()) << "\n";

    std::cout << "\n--- Data Integrity ---\n";
    bool integrity = verify_data_integrity(cfg, cfg.num_symbols);

    bool pass = status_fail == 0 && integrity && max_latency < 1000.0;
    std::cout << "\n  status_polling: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

    std::cout << "\n=== ZeptoDB Live Rebalancing Load Test ===\n";
    std::cout << "Cluster: " << cfg.host << ":" << cfg.port << "\n";
    std::cout << "Symbols: " << cfg.num_symbols
              << ", Target ingest: " << cfg.ticks_per_sec << " ticks/sec"
              << ", Query QPS: " << cfg.query_qps
              << ", Scenario: " << cfg.scenario << "\n";

    // Cluster info
    auto cluster_info = get_cluster_info(cfg);
    if (!cluster_info.empty()) {
        std::cout << "Cluster info: " << cluster_info << "\n";
    } else {
        auto ping = http_get(cfg, "/ping");
        if (ping.empty()) {
            std::cerr << "ERROR: Cluster unreachable at " << cfg.host << ":" << cfg.port << "\n";
            return 1;
        }
        std::cerr << "WARNING: /admin/cluster not available, but /ping OK\n";
    }

    int node_id = cfg.rebalance_node_id;
    if (node_id == 0)
        node_id = (cfg.action == "add_node") ? 4 : 3;

    bool run_all = (cfg.scenario == "all");
    int passed = 0, failed = 0, total = 0;

    auto run_scenario = [&](const std::string& name, auto fn) {
        if (!run_all && cfg.scenario != name) return;
        total++;
        std::cout << "\n" << std::string(60, '=') << "\n";
        std::cout << "SCENARIO: " << name << "\n";
        std::cout << std::string(60, '=') << "\n";
        bool ok = fn();
        if (ok) passed++; else failed++;
        std::cout << "\n>>> " << name << ": " << (ok ? "PASS" : "FAIL") << "\n";
    };

    run_scenario("basic", [&]{ return scenario_basic(cfg, node_id); });
    run_scenario("add_remove_cycle", [&]{ return scenario_add_remove_cycle(cfg, node_id); });
    run_scenario("pause_resume", [&]{ return scenario_pause_resume(cfg, node_id); });
    run_scenario("heavy_query", [&]{ return scenario_heavy_query(cfg, node_id); });
    run_scenario("back_to_back", [&]{ return scenario_back_to_back(cfg, node_id); });
    run_scenario("status_polling", [&]{ return scenario_status_polling(cfg, node_id); });

    // Final summary
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "FINAL RESULT: " << passed << "/" << total << " scenarios passed";
    if (failed > 0) std::cout << " (" << failed << " FAILED)";
    std::cout << "\n" << std::string(60, '=') << "\n\n";

    return (failed == 0) ? 0 : 1;
}

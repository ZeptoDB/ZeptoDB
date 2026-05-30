// ============================================================================
// Agent Memory Benchmark
// ============================================================================
// Measures the first Agent Memory performance wedge:
//   1. filtered top-K memory search latency
//   2. token-budget context assembly latency
//   3. exact and semantic cache lookup latency
//   4. sidecar snapshot save/load time and bytes per memory item

#include "zeptodb/ai/agent_memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using zeptodb::ai::AgentMemoryStore;
using zeptodb::ai::AgentMemoryAnnConfig;
using zeptodb::ai::AgentMemoryAnnMode;
using zeptodb::ai::CacheEntry;
using zeptodb::ai::CacheLookup;
using zeptodb::ai::ContextRequest;
using zeptodb::ai::MemoryQuery;
using zeptodb::ai::MemoryRecord;

using clock_t_ = std::chrono::steady_clock;

struct Args {
    size_t records = 10'000;
    size_t dim = 128;
    size_t iters = 100;
    bool sweep = false;
    bool skip_snapshot = false;
    bool compare_ann = false;
    bool read_only_search = false;
    bool semantic_fixture = false;
    double ann_threshold_ms = 10.0;
    size_t recall_queries = 1;
    std::vector<size_t> sweep_records = {10'000, 100'000, 1'000'000};
    std::string ann_mode = "off";
    std::string ann_label;
    size_t ann_min_records = 50'000;
    size_t ann_max_candidates = 50'000;
    size_t ann_oversample = 8;
    size_t ann_tables = 8;
    size_t ann_bits = 12;
    size_t ann_terms = 8;
    size_t ann_probe_radius = 2;
    size_t ann_hnsw_m = 16;
    size_t ann_hnsw_ef_construction = 200;
    size_t ann_hnsw_ef_search = 64;
};

struct BenchSummary {
    size_t records = 0;
    double search_p50_ms = 0.0;
    double context_p50_ms = 0.0;
    double exact_p50_ms = 0.0;
    double semantic_p50_ms = 0.0;
    double save_ms = 0.0;
    double load_ms = 0.0;
    double bytes_per_item = 0.0;
    double ann_build_ms = 0.0;
    double recall_at_k = 1.0;
    double recall_min_at_k = 1.0;
    size_t recall_queries = 1;
    size_t ann_indexed_vectors = 0;
    uint64_t ann_search_count = 0;
    uint64_t ann_fallback_count = 0;
    std::string ann_mode = "off";
    size_t ann_max_candidates = 0;
    size_t ann_oversample = 0;
    size_t ann_tables = 0;
    size_t ann_bits = 0;
    size_t ann_probe_radius = 0;
    size_t ann_hnsw_m = 0;
    size_t ann_hnsw_ef_construction = 0;
    size_t ann_hnsw_ef_search = 0;
};

static double elapsed_us(const clock_t_::time_point& start) {
    return std::chrono::duration<double, std::micro>(
        clock_t_::now() - start).count();
}

static double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = p * static_cast<double>(values.size() - 1);
    const auto idx = static_cast<size_t>(pos);
    const auto next = std::min(idx + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(idx);
    return values[idx] * (1.0 - frac) + values[next] * frac;
}

static void print_latency(const std::string& name,
                          const std::vector<double>& samples_us) {
    std::cout << std::left << std::setw(32) << name
              << " p50=" << std::right << std::setw(9) << std::fixed
              << std::setprecision(2) << percentile(samples_us, 0.50) / 1000.0
              << " ms  p95=" << std::setw(9)
              << percentile(samples_us, 0.95) / 1000.0 << " ms\n";
}

static uint64_t mix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

static std::vector<float> make_embedding(size_t dim, size_t seed) {
    std::vector<float> out(dim);
    for (size_t i = 0; i < dim; ++i) {
        const uint64_t h = mix64((static_cast<uint64_t>(seed) << 32U) ^
                                 static_cast<uint64_t>(i));
        const double unit = static_cast<double>(h & 0x00FFFFFFULL) /
            static_cast<double>(0x00FFFFFFULL);
        out[i] = static_cast<float>(unit * 2.0 - 1.0);
    }
    return out;
}

static std::vector<size_t> parse_record_list(const std::string& csv) {
    std::vector<size_t> records;
    std::stringstream stream(csv);
    std::string item;
    while (std::getline(stream, item, ',')) {
        char* end = nullptr;
        const auto value = std::strtoull(item.c_str(), &end, 10);
        bool has_trailing_junk = false;
        while (end != nullptr && *end != '\0') {
            if (std::isspace(static_cast<unsigned char>(*end)) == 0) {
                has_trailing_junk = true;
                break;
            }
            ++end;
        }
        if (end == item.c_str() || value == 0 ||
            value > std::numeric_limits<size_t>::max() || has_trailing_junk) {
            std::cerr << "invalid --sweep-records item: " << item << "\n";
            std::exit(1);
        }
        records.push_back(static_cast<size_t>(value));
    }
    if (records.empty()) {
        std::cerr << "--sweep-records must include at least one record count\n";
        std::exit(1);
    }
    return records;
}

static Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--records" && i + 1 < argc) {
            args.records = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--dim" && i + 1 < argc) {
            args.dim = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--iters" && i + 1 < argc) {
            args.iters = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--sweep") {
            args.sweep = true;
        } else if (arg == "--compare-ann") {
            args.compare_ann = true;
        } else if (arg == "--semantic-fixture") {
            args.semantic_fixture = true;
        } else if (arg == "--sweep-records" && i + 1 < argc) {
            args.sweep_records = parse_record_list(argv[++i]);
        } else if (arg == "--skip-snapshot") {
            args.skip_snapshot = true;
        } else if (arg == "--ann-threshold-ms" && i + 1 < argc) {
            args.ann_threshold_ms = std::atof(argv[++i]);
        } else if (arg == "--recall-queries" && i + 1 < argc) {
            args.recall_queries = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann" && i + 1 < argc) {
            args.ann_mode = argv[++i];
        } else if (arg == "--ann-min-records" && i + 1 < argc) {
            args.ann_min_records = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-max-candidates" && i + 1 < argc) {
            args.ann_max_candidates = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-oversample" && i + 1 < argc) {
            args.ann_oversample = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-tables" && i + 1 < argc) {
            args.ann_tables = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-bits" && i + 1 < argc) {
            args.ann_bits = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-terms" && i + 1 < argc) {
            args.ann_terms = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-probe-radius" && i + 1 < argc) {
            args.ann_probe_radius = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-hnsw-m" && i + 1 < argc) {
            args.ann_hnsw_m = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-hnsw-ef-construction" && i + 1 < argc) {
            args.ann_hnsw_ef_construction =
                static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--ann-hnsw-ef-search" && i + 1 < argc) {
            args.ann_hnsw_ef_search =
                static_cast<size_t>(std::atoll(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--records 10000] [--dim 128] [--iters 100]\n"
                      << "       [--sweep] [--compare-ann]\n"
                      << "       [--semantic-fixture]\n"
                      << "       [--sweep-records 10000,100000,1000000]\n"
                      << "       [--skip-snapshot]\n"
                      << "       [--recall-queries 1]\n"
                      << "       [--ann off|auto|sparse_projection|hnsw]\n"
                      << "       [--ann-min-records 50000]\n"
                      << "       [--ann-max-candidates 50000]\n"
                      << "       [--ann-oversample 8]\n"
                      << "       [--ann-tables 8] [--ann-bits 12]\n"
                      << "       [--ann-terms 8] [--ann-probe-radius 2]\n"
                      << "       [--ann-hnsw-m 16]\n"
                      << "       [--ann-hnsw-ef-construction 200]\n"
                      << "       [--ann-hnsw-ef-search 64]\n"
                      << "       [--ann-threshold-ms 10.0]\n";
            std::exit(0);
        }
    }
    args.records = std::max<size_t>(1, args.records);
    args.dim = std::max<size_t>(1, args.dim);
    args.iters = std::max<size_t>(1, args.iters);
    args.ann_threshold_ms = std::max(0.1, args.ann_threshold_ms);
    args.recall_queries = std::max<size_t>(1, args.recall_queries);
    args.ann_min_records = std::max<size_t>(1, args.ann_min_records);
    args.ann_max_candidates = std::max<size_t>(1, args.ann_max_candidates);
    args.ann_oversample = std::max<size_t>(1, args.ann_oversample);
    args.ann_tables = std::max<size_t>(1, args.ann_tables);
    args.ann_bits = std::max<size_t>(1, args.ann_bits);
    args.ann_terms = std::max<size_t>(1, args.ann_terms);
    args.ann_hnsw_m = std::max<size_t>(2, args.ann_hnsw_m);
    args.ann_hnsw_ef_construction =
        std::max<size_t>(args.ann_hnsw_m, args.ann_hnsw_ef_construction);
    args.ann_hnsw_ef_search = std::max<size_t>(1, args.ann_hnsw_ef_search);
    return args;
}

static AgentMemoryAnnMode parse_ann_mode(const std::string& value) {
    if (value == "off") return AgentMemoryAnnMode::Off;
    if (value == "auto") return AgentMemoryAnnMode::Auto;
    if (value == "sparse_projection" || value == "projection") {
        return AgentMemoryAnnMode::SparseProjection;
    }
    if (value == "hnsw") {
        if (!zeptodb::ai::hnsw_ann_available()) {
            std::cerr << "invalid --ann mode: hnsw requires "
                      << "ZEPTO_ENABLE_HNSWLIB=ON\n";
            std::exit(1);
        }
        return AgentMemoryAnnMode::Hnsw;
    }
    std::cerr << "invalid --ann mode: " << value << "\n";
    std::exit(1);
}

static std::string ann_label(const Args& args) {
    return args.ann_label.empty() ? args.ann_mode : args.ann_label;
}

static AgentMemoryAnnConfig make_ann_config(const Args& args) {
    AgentMemoryAnnConfig ann_config;
    ann_config.mode = parse_ann_mode(args.ann_mode);
    ann_config.min_records = args.ann_min_records;
    ann_config.oversample = args.ann_oversample;
    ann_config.index.max_candidates = args.ann_max_candidates;
    ann_config.index.tables = args.ann_tables;
    ann_config.index.bits_per_table = args.ann_bits;
    ann_config.index.terms_per_bit = args.ann_terms;
    ann_config.index.probe_radius = args.ann_probe_radius;
    ann_config.index.hnsw_m = args.ann_hnsw_m;
    ann_config.index.hnsw_ef_construction = args.ann_hnsw_ef_construction;
    ann_config.index.hnsw_ef_search = args.ann_hnsw_ef_search;
    return ann_config;
}

static void configure_ann(AgentMemoryStore* store, const Args& args) {
    store->set_ann_config(make_ann_config(args));
}

static void seed_store(AgentMemoryStore* store, const Args& args) {
    store->reserve_memory_capacity(args.records, std::min<size_t>(args.records, 512));
    const int64_t now = AgentMemoryStore::now_ns();
    for (size_t i = 0; i < args.records; ++i) {
        MemoryRecord r;
        r.memory_id = "mem_" + std::to_string(i);
        r.tenant_id = i % 2 == 0 ? "tenant_a" : "tenant_b";
        r.namespace_id = "agent";
        r.user_id = "user_" + std::to_string(i % 16);
        r.session_id = "session_" + std::to_string(i % 64);
        r.agent_id = "agent_" + std::to_string(i % 4);
        r.type = "memory";
        r.content = "memory item " + std::to_string(i) + " with reusable context";
        r.metadata_json = R"({"source":"bench","trusted":true})";
        r.embedding = make_embedding(args.dim, i);
        r.token_count = 8 + static_cast<int64_t>(i % 16);
        if (args.semantic_fixture) {
            r.importance = 0.0;
            r.created_at_ns = now;
            r.pinned = false;
        } else {
            r.importance = static_cast<double>(i % 10) / 10.0;
            r.created_at_ns = now - static_cast<int64_t>(i) * 1'000'000;
            r.pinned = i % 997 == 0;
        }
        const auto stored = store->put_memory(std::move(r));
        if (!stored.ok) {
            std::cerr << "seed failed: " << stored.error << "\n";
            std::exit(2);
        }
    }

    for (size_t i = 0; i < std::min<size_t>(args.records, 512); ++i) {
        CacheEntry c;
        c.cache_id = "cache_" + std::to_string(i);
        c.tenant_id = "tenant_a";
        c.namespace_id = "agent";
        c.prompt = "question " + std::to_string(i);
        c.response = "cached answer " + std::to_string(i);
        c.embedding = make_embedding(args.dim, i * 17 + 5);
        c.token_count = 16;
        const auto stored = store->store_cache(std::move(c));
        if (!stored.ok) {
            std::cerr << "cache seed failed: " << stored.error << "\n";
            std::exit(2);
        }
    }
}

template <typename Fn>
static std::vector<double> measure(size_t iters, Fn fn) {
    std::vector<double> samples;
    samples.reserve(iters);
    for (size_t i = 0; i < iters; ++i) {
        const auto start = clock_t_::now();
        fn(i);
        samples.push_back(elapsed_us(start));
    }
    return samples;
}

static uint64_t directory_bytes(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        total += static_cast<uint64_t>(entry.file_size(ec));
    }
    return total;
}

static size_t sweep_iters(size_t records, size_t requested_iters) {
    if (records >= 1'000'000) return std::min<size_t>(requested_iters, 5);
    if (records >= 100'000) return std::min<size_t>(requested_iters, 20);
    return requested_iters;
}

struct RecallSummary {
    double average = 1.0;
    double minimum = 1.0;
};

static RecallSummary measure_recall(AgentMemoryStore* store,
                                    const Args& args,
                                    const MemoryQuery& base_query) {
    const AgentMemoryAnnMode mode = parse_ann_mode(args.ann_mode);
    if (mode == AgentMemoryAnnMode::Off) return {};

    double total = 0.0;
    double min_recall = 1.0;
    for (size_t i = 0; i < args.recall_queries; ++i) {
        auto exact_query = base_query;
        exact_query.query_embedding = make_embedding(args.dim, 42 + i * 7919);
        exact_query.force_scan = true;
        exact_query.update_access = false;

        auto ann_query = exact_query;
        ann_query.force_scan = false;

        const auto exact = store->search(exact_query);
        const auto approx = store->search(ann_query);
        std::unordered_set<std::string> exact_ids;
        exact_ids.reserve(exact.size());
        for (const auto& row : exact) exact_ids.insert(row.record.memory_id);

        size_t overlap = 0;
        for (const auto& row : approx) {
            if (exact_ids.find(row.record.memory_id) != exact_ids.end()) {
                ++overlap;
            }
        }
        const double recall = exact.empty()
            ? 1.0
            : static_cast<double>(overlap) / static_cast<double>(exact.size());
        total += recall;
        min_recall = std::min(min_recall, recall);
    }
    return {total / static_cast<double>(args.recall_queries), min_recall};
}

static BenchSummary run_on_store(AgentMemoryStore* store, Args args) {
    const AgentMemoryAnnConfig ann_config = make_ann_config(args);
    configure_ann(store, args);

    const std::vector<float> query = make_embedding(args.dim, 42);
    MemoryQuery search_query;
    search_query.tenant_id = "tenant_a";
    search_query.namespace_id = "agent";
    search_query.query_embedding = query;
    search_query.limit = 16;
    search_query.update_access = !args.read_only_search;

    ContextRequest context_query;
    context_query.tenant_id = "tenant_a";
    context_query.namespace_id = "agent";
    context_query.query_embedding = query;
    context_query.limit = 64;
    context_query.token_budget = 512;
    context_query.update_access = !args.read_only_search;

    CacheLookup exact;
    exact.tenant_id = "tenant_a";
    exact.namespace_id = "agent";
    exact.prompt = " question 42 ";

    CacheLookup semantic;
    semantic.tenant_id = "tenant_a";
    semantic.namespace_id = "agent";
    semantic.prompt = "semantic fallback";
    semantic.embedding = make_embedding(args.dim, 5);
    semantic.semantic_threshold = 0.0;

    std::cout << "Agent Memory benchmark"
              << " records=" << args.records
              << " dim=" << args.dim
              << " iters=" << args.iters
              << " ann=" << ann_label(args)
              << " ann_candidates=" << args.ann_max_candidates
              << " ann_oversample=" << args.ann_oversample
              << " ann_tables=" << args.ann_tables
              << " ann_bits=" << args.ann_bits
              << " ann_probe_radius=" << args.ann_probe_radius
              << " hnsw_m=" << args.ann_hnsw_m
              << " hnsw_efc=" << args.ann_hnsw_ef_construction
              << " hnsw_efs=" << args.ann_hnsw_ef_search
              << " recall_queries=" << args.recall_queries
              << (args.semantic_fixture ? " semantic_fixture=true" : "")
              << (args.read_only_search ? " read_only_search=true" : "")
              << (args.skip_snapshot ? " skip_snapshot=true" : "")
              << "\n\n";

    BenchSummary summary;
    summary.records = args.records;
    summary.ann_mode = ann_label(args);
    summary.ann_max_candidates = args.ann_max_candidates;
    summary.ann_oversample = args.ann_oversample;
    summary.ann_tables = args.ann_tables;
    summary.ann_bits = args.ann_bits;
    summary.ann_probe_radius = args.ann_probe_radius;
    summary.ann_hnsw_m = args.ann_hnsw_m;
    summary.ann_hnsw_ef_construction = args.ann_hnsw_ef_construction;
    summary.ann_hnsw_ef_search = args.ann_hnsw_ef_search;
    summary.recall_queries = args.recall_queries;
    if (ann_config.mode == AgentMemoryAnnMode::SparseProjection ||
        ann_config.mode == AgentMemoryAnnMode::Hnsw ||
        (ann_config.mode == AgentMemoryAnnMode::Auto &&
         args.records >= args.ann_min_records)) {
        const auto build_start = clock_t_::now();
        auto built = store->rebuild_ann_index();
        summary.ann_build_ms = elapsed_us(build_start) / 1000.0;
        if (!built.ok) {
            std::cerr << "ANN rebuild failed: " << built.error << "\n";
            std::exit(5);
        }
        const auto stats = store->stats();
        summary.ann_indexed_vectors = stats.ann_indexed_vectors;
        std::cout << std::left << std::setw(32) << "ann rebuild"
                  << " " << std::fixed << std::setprecision(2)
                  << summary.ann_build_ms << " ms"
                  << " indexed=" << summary.ann_indexed_vectors
                  << " partitions=" << stats.ann_partitions
                  << " buckets=" << stats.ann_buckets
                  << " max_bucket=" << stats.ann_max_bucket_size << "\n";
    }
    const auto stats_before_search = store->stats();

    if (ann_config.mode != AgentMemoryAnnMode::Off) {
        const auto recall = measure_recall(store, args, search_query);
        summary.recall_at_k = recall.average;
        summary.recall_min_at_k = recall.minimum;
        std::cout << std::left << std::setw(32) << "ann recall@K"
                  << " " << std::fixed << std::setprecision(3)
                  << summary.recall_at_k
                  << " min=" << summary.recall_min_at_k
                  << " queries=" << args.recall_queries
                  << " k=" << search_query.limit << "\n";
    }

    auto search_samples = measure(args.iters, [&](size_t) {
        auto rows = store->search(search_query);
        if (rows.empty()) std::exit(3);
    });
    print_latency("memory search top-K", search_samples);

    auto context_samples = measure(args.iters, [&](size_t) {
        auto context = store->get_context(context_query);
        if (context.memories.empty()) std::exit(3);
    });
    print_latency("context assembly", context_samples);

    auto exact_samples = measure(args.iters, [&](size_t) {
        auto hit = store->lookup_cache(exact);
        if (!hit.hit || !hit.exact) std::exit(3);
    });
    print_latency("exact cache lookup", exact_samples);

    auto semantic_samples = measure(args.iters, [&](size_t) {
        auto hit = store->lookup_cache(semantic);
        if (!hit.hit) std::exit(3);
    });
    print_latency("semantic cache lookup", semantic_samples);

    summary.search_p50_ms = percentile(search_samples, 0.50) / 1000.0;
    summary.context_p50_ms = percentile(context_samples, 0.50) / 1000.0;
    summary.exact_p50_ms = percentile(exact_samples, 0.50) / 1000.0;
    summary.semantic_p50_ms = percentile(semantic_samples, 0.50) / 1000.0;
    const auto stats_after_search = store->stats();
    summary.ann_search_count =
        stats_after_search.ann_search_count - stats_before_search.ann_search_count;
    summary.ann_fallback_count =
        stats_after_search.ann_fallback_count - stats_before_search.ann_fallback_count;

    if (args.skip_snapshot) {
        std::cout << "\n";
        return summary;
    }

    const fs::path dir = fs::temp_directory_path() /
        ("zeptodb_agent_memory_bench_" + std::to_string(AgentMemoryStore::now_ns()));
    const auto save_start = clock_t_::now();
    auto saved = store->save_to_directory(dir.string());
    const double save_ms = elapsed_us(save_start) / 1000.0;
    if (!saved.ok) {
        std::cerr << "save failed: " << saved.error << "\n";
        std::exit(4);
    }

    AgentMemoryStore loaded;
    const auto load_start = clock_t_::now();
    auto loaded_result = loaded.load_from_directory(dir.string());
    const double load_ms = elapsed_us(load_start) / 1000.0;
    if (!loaded_result.ok) {
        std::cerr << "load failed: " << loaded_result.error << "\n";
        std::exit(4);
    }

    const uint64_t bytes = directory_bytes(dir);
    summary.save_ms = save_ms;
    summary.load_ms = load_ms;
    summary.bytes_per_item = static_cast<double>(bytes) / static_cast<double>(args.records);
    std::cout << "\n";
    std::cout << std::left << std::setw(32) << "snapshot save"
              << " " << std::fixed << std::setprecision(2) << save_ms << " ms\n";
    std::cout << std::left << std::setw(32) << "snapshot load"
              << " " << std::fixed << std::setprecision(2) << load_ms << " ms\n";
    std::cout << std::left << std::setw(32) << "snapshot bytes/item"
              << " " << std::fixed << std::setprecision(1)
              << static_cast<double>(bytes) / static_cast<double>(args.records)
              << " bytes\n";

    std::error_code ec;
    fs::remove_all(dir, ec);
    return summary;
}

static BenchSummary run_once(Args args) {
    AgentMemoryStore store;
    configure_ann(&store, args);
    seed_store(&store, args);
    return run_on_store(&store, std::move(args));
}

static std::vector<Args> compare_profiles(Args args) {
    args.skip_snapshot = true;
    args.read_only_search = true;

    Args exact = args;
    exact.ann_label = "exact_scan";
    exact.ann_mode = "off";
    exact.ann_max_candidates = 0;
    exact.ann_oversample = 0;
    exact.ann_tables = 0;
    exact.ann_bits = 0;
    exact.ann_probe_radius = 0;
    exact.ann_hnsw_m = 0;
    exact.ann_hnsw_ef_construction = 0;
    exact.ann_hnsw_ef_search = 0;

    Args sparse_fast = args;
    sparse_fast.ann_label = "sparse_fast";
    sparse_fast.ann_mode = "sparse_projection";
    sparse_fast.ann_min_records = 1;
    sparse_fast.ann_max_candidates = 4'000;
    sparse_fast.ann_oversample = 16;
    sparse_fast.ann_tables = 4;
    sparse_fast.ann_bits = 12;
    sparse_fast.ann_terms = 8;
    sparse_fast.ann_probe_radius = 2;
    sparse_fast.ann_hnsw_m = 0;
    sparse_fast.ann_hnsw_ef_construction = 0;
    sparse_fast.ann_hnsw_ef_search = 0;

    Args sparse_wide = args;
    sparse_wide.ann_label = "sparse_wide";
    sparse_wide.ann_mode = "sparse_projection";
    sparse_wide.ann_min_records = 1;
    sparse_wide.ann_max_candidates = 50'000;
    sparse_wide.ann_oversample = 1024;
    sparse_wide.ann_tables = 16;
    sparse_wide.ann_bits = 12;
    sparse_wide.ann_terms = 8;
    sparse_wide.ann_probe_radius = 2;
    sparse_wide.ann_hnsw_m = 0;
    sparse_wide.ann_hnsw_ef_construction = 0;
    sparse_wide.ann_hnsw_ef_search = 0;

    std::vector<Args> profiles{exact, sparse_fast, sparse_wide};
#ifdef ZEPTO_ENABLE_HNSWLIB
    Args hnsw_fast = args;
    hnsw_fast.ann_label = "hnsw_fast";
    hnsw_fast.ann_mode = "hnsw";
    hnsw_fast.ann_max_candidates = 50'000;
    hnsw_fast.ann_oversample = 4;
    hnsw_fast.ann_hnsw_m = 16;
    hnsw_fast.ann_hnsw_ef_construction = 100;
    hnsw_fast.ann_hnsw_ef_search = 64;
    profiles.push_back(hnsw_fast);

    Args hnsw_recall = args;
    hnsw_recall.ann_label = "hnsw_recall";
    hnsw_recall.ann_mode = "hnsw";
    hnsw_recall.ann_max_candidates = 50'000;
    hnsw_recall.ann_oversample = 16;
    hnsw_recall.ann_hnsw_m = 24;
    hnsw_recall.ann_hnsw_ef_construction = 200;
    hnsw_recall.ann_hnsw_ef_search = 200;
    profiles.push_back(hnsw_recall);
#endif

    return profiles;
}

static std::vector<BenchSummary> run_compare(Args args) {
    AgentMemoryStore store;
    seed_store(&store, args);

    std::vector<BenchSummary> summaries;
    const auto profiles = compare_profiles(std::move(args));
    summaries.reserve(profiles.size());
    for (const auto& profile : profiles) {
        summaries.push_back(run_on_store(&store, profile));
    }
    return summaries;
}

static void print_decision_table(const std::vector<BenchSummary>& summaries,
                                 double ann_threshold_ms) {
    std::cout << "\n=== ANN Decision Summary ===\n";
    std::cout << std::left << std::setw(12) << "records"
              << std::setw(20) << "ann"
              << std::setw(13) << "cand"
              << std::setw(8) << "os"
              << std::setw(8) << "tbl"
              << std::setw(8) << "bits"
              << std::setw(8) << "rad"
              << std::setw(8) << "M"
              << std::setw(8) << "efc"
              << std::setw(8) << "efs"
              << std::right << std::setw(14) << "search p50"
              << std::setw(14) << "context p50"
              << std::setw(14) << "semantic p50"
              << std::setw(14) << "ann build"
              << std::setw(14) << "recall avg"
              << std::setw(14) << "recall min"
              << std::setw(10) << "rq"
              << std::setw(14) << "fallbacks"
              << std::setw(14) << "ANN?"
              << "\n";
    for (const auto& s : summaries) {
        const bool ann_candidate = s.search_p50_ms > ann_threshold_ms ||
            s.context_p50_ms > ann_threshold_ms;
        std::cout << std::left << std::setw(12) << s.records
                  << std::setw(20) << s.ann_mode
                  << std::setw(13) << s.ann_max_candidates
                  << std::setw(8) << s.ann_oversample
                  << std::setw(8) << s.ann_tables
                  << std::setw(8) << s.ann_bits
                  << std::setw(8) << s.ann_probe_radius
                  << std::setw(8) << s.ann_hnsw_m
                  << std::setw(8) << s.ann_hnsw_ef_construction
                  << std::setw(8) << s.ann_hnsw_ef_search
                  << std::right << std::setw(11) << std::fixed << std::setprecision(2)
                  << s.search_p50_ms << " ms"
                  << std::setw(11) << s.context_p50_ms << " ms"
                  << std::setw(11) << s.semantic_p50_ms << " ms"
                  << std::setw(11) << s.ann_build_ms << " ms"
                  << std::setw(14) << std::fixed << std::setprecision(3)
                  << s.recall_at_k
                  << std::setw(14) << s.recall_min_at_k
                  << std::setw(10) << s.recall_queries
                  << std::setw(14) << s.ann_fallback_count
                  << std::setw(14) << (ann_candidate ? "candidate" : "scan-ok")
                  << "\n";
    }
    std::cout << "threshold: p50 search/context > "
              << std::fixed << std::setprecision(2)
              << ann_threshold_ms << " ms\n";
}

int main(int argc, char* argv[]) {
    const Args args = parse_args(argc, argv);
    if (!args.sweep) {
        std::vector<BenchSummary> summaries;
        if (args.compare_ann) {
            summaries = run_compare(args);
        } else {
            summaries.push_back(run_once(args));
        }
        print_decision_table(summaries, args.ann_threshold_ms);
        return 0;
    }

    std::vector<BenchSummary> summaries;
    for (const size_t records : args.sweep_records) {
        Args run_args = args;
        run_args.records = records;
        run_args.iters = sweep_iters(records, args.iters);
        if (run_args.compare_ann) {
            auto run_summaries = run_compare(run_args);
            summaries.insert(summaries.end(),
                             std::make_move_iterator(run_summaries.begin()),
                             std::make_move_iterator(run_summaries.end()));
        } else {
            summaries.push_back(run_once(run_args));
        }
    }
    print_decision_table(summaries, args.ann_threshold_ms);
    return 0;
}

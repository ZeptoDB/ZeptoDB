// ============================================================================
// Benchmark: SIMD vs Scalar vs JIT + Phase B v2 최적화 변형
// ============================================================================
// Phase B v1: Highway SIMD + LLVM JIT 성능 비교
// Phase B v2: 최적화 변형 추가 비교
//   1. filter_gt_i64:  scalar → SIMD v1(SelectionVec) → SIMD v2(BitMask)
//   2. sum_i64:        scalar → SIMD v1(4x) → fast(scalar 4-way) → SIMD v2(8x+pf)
//   3. vwap:           scalar → SIMD v1(2x) → fused(4x+pf)
//   4. JIT filter:     per-row v1(Os) → per-row v2(O3) → bulk(O3 루프 IR)
// ============================================================================

#include "zeptodb/execution/vectorized_engine.h"
#include "zeptodb/execution/jit_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace zeptodb::execution;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 타이밍 헬퍼
// ============================================================================
template<typename Fn>
int64_t time_us(Fn&& fn, int warmup = 2, int iters = 5) {
    for (int i = 0; i < warmup; ++i) fn();

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto t1 = Clock::now();

    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / iters;
}

// p50/p99 측정용 (반복 횟수 증가)
template<typename Fn>
std::pair<int64_t, int64_t> time_percentiles(Fn&& fn, int iters = 20) {
    std::vector<int64_t> samples;
    samples.reserve(iters);

    // 워밍업
    for (int i = 0; i < 3; ++i) fn();

    for (int i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
        );
    }

    std::sort(samples.begin(), samples.end());
    int64_t p50 = samples[iters / 2];
    int64_t p99 = samples[static_cast<size_t>(iters * 0.99)];
    return {p50, p99};
}

// ============================================================================
// 테스트 데이터 생성
// ============================================================================
struct TestData {
    std::vector<int64_t> prices;
    std::vector<int64_t> volumes;
    size_t n;

    explicit TestData(size_t n) : n(n) {
        prices.resize(n);
        volumes.resize(n);

        std::mt19937_64 rng(42);
        std::uniform_int_distribution<int64_t> price_dist(1000, 200000);
        std::uniform_int_distribution<int64_t> vol_dist(1, 10000);

        for (size_t i = 0; i < n; ++i) {
            prices[i]  = price_dist(rng);
            volumes[i] = vol_dist(rng);
        }
    }
};

// ============================================================================
// Scalar implementations (비교 기준)
// ============================================================================
__attribute__((noinline))
int64_t scalar_sum(const int64_t* data, size_t n) {
    int64_t sum = 0;
    #pragma clang loop vectorize(disable) interleave(disable)
    for (size_t i = 0; i < n; ++i) sum += data[i];
    return sum;
}

__attribute__((noinline))
size_t scalar_filter_gt(const int64_t* data, size_t n, int64_t threshold,
                         uint32_t* out) {
    size_t count = 0;
    #pragma clang loop vectorize(disable) interleave(disable)
    for (size_t i = 0; i < n; ++i) {
        if (data[i] > threshold) out[count++] = static_cast<uint32_t>(i);
    }
    return count;
}

__attribute__((noinline))
double scalar_vwap(const int64_t* prices, const int64_t* volumes, size_t n) {
    __int128 pv = 0;
    int64_t  v  = 0;
    #pragma clang loop vectorize(disable) interleave(disable)
    for (size_t i = 0; i < n; ++i) {
        pv += static_cast<__int128>(prices[i]) * volumes[i];
        v  += volumes[i];
    }
    return v ? static_cast<double>(pv) / static_cast<double>(v) : 0.0;
}

// ============================================================================
// 벤치마크 출력 헬퍼
// ============================================================================
void print_bench(const std::string& op, size_t rows,
                 int64_t scalar_us, int64_t simd_us) {
    double speedup = static_cast<double>(scalar_us) / static_cast<double>(simd_us);
    std::string rows_str;
    if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
    else if (rows >= 1'000) rows_str = std::to_string(rows / 1'000) + "K";
    else rows_str = std::to_string(rows);

    std::cout << "[SIMD] " << std::left << std::setw(18) << op
              << " " << std::right << std::setw(4) << rows_str << " rows:"
              << "  scalar=" << std::setw(6) << scalar_us << "μs"
              << "  simd="   << std::setw(6) << simd_us   << "μs"
              << "  speedup=" << std::fixed << std::setprecision(1)
              << speedup << "x\n";
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::cout << "=============================================================\n";
    std::cout << " ZeptoDB Phase B: SIMD + JIT Benchmark\n";
    std::cout << "=============================================================\n\n";

    // --------------------------------------------------------
    // Part 1: SIMD v1 vs Scalar (기존 벤치마크 — 하위 호환)
    // --------------------------------------------------------
    std::cout << "--- [Part 1] SIMD v1 vs Scalar ---\n";

    const int64_t FILTER_THRESHOLD = 100000;

    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);
        std::vector<uint32_t> out_buf(rows);
        volatile int64_t chk1 = 0;
        volatile int64_t chk2 = 0;
        volatile double  chk3 = 0.0;
        volatile size_t  chk4 = 0;
        volatile size_t  chk5 = 0;

        // --- sum_i64 ---
        int64_t sc_sum = time_us([&]{
            chk1 = scalar_sum(td.prices.data(), rows);
        });
        int64_t si_sum = time_us([&]{
            chk2 = sum_i64(td.prices.data(), rows);
        });
        (void)chk1; (void)chk2;
        print_bench("sum_i64", rows, sc_sum, si_sum);

        // --- filter_gt_i64 ---
        int64_t sc_flt = time_us([&]{
            chk4 = scalar_filter_gt(td.prices.data(), rows, FILTER_THRESHOLD, out_buf.data());
        });

        SelectionVector sel(rows);
        int64_t si_flt = time_us([&]{
            filter_gt_i64(td.prices.data(), rows, FILTER_THRESHOLD, sel);
            chk5 = sel.size();
        });
        (void)chk4; (void)chk5;
        print_bench("filter_gt_i64", rows, sc_flt, si_flt);

        // --- vwap ---
        int64_t sc_vwap = time_us([&]{
            chk3 = scalar_vwap(td.prices.data(), td.volumes.data(), rows);
        });
        int64_t si_vwap = time_us([&]{
            chk3 = vwap(td.prices.data(), td.volumes.data(), rows);
        });
        (void)chk3;
        print_bench("vwap", rows, sc_vwap, si_vwap);

        std::cout << "\n";
    }

    // --------------------------------------------------------
    // Part 2: JIT Engine v1 (기존)
    // --------------------------------------------------------
    std::cout << "--- [Part 2] JIT Compiled Filter (v1 per-row) ---\n";

    JITEngine jit;
    if (!jit.initialize()) {
        std::cerr << "[JIT] 초기화 실패: " << jit.last_error() << "\n";
        return 1;
    }

    const std::string expr = "price > 100000 AND volume > 5000";

    FilterFn filter_fn = nullptr;
    auto compile_start = Clock::now();
    filter_fn = jit.compile(expr);
    auto compile_end = Clock::now();

    if (!filter_fn) {
        std::cerr << "[JIT] 컴파일 실패: " << jit.last_error() << "\n";
        return 1;
    }

    int64_t compile_us = std::chrono::duration_cast<std::chrono::microseconds>(
        compile_end - compile_start).count();

    std::cout << "[JIT] Expression: \"" << expr << "\"\n";
    std::cout << "[JIT] Compile time (v1, O3): " << compile_us << "μs\n\n";

    auto cpp_lambda = [](int64_t price, int64_t volume) -> bool {
        return price > 100000 && volume > 5000;
    };

    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        volatile size_t jit_count = 0;
        int64_t jit_exec_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (filter_fn(td.prices[i], td.volumes[i])) ++cnt;
            }
            jit_count = cnt;
        });

        volatile size_t cpp_count = 0;
        int64_t cpp_exec_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (cpp_lambda(td.prices[i], td.volumes[i])) ++cnt;
            }
            cpp_count = cnt;
        });

        FilterFn cpp_fptr = +[](int64_t price, int64_t volume) -> bool {
            return price > 100000 && volume > 5000;
        };
        volatile size_t fptr_count = 0;
        int64_t fptr_exec_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (cpp_fptr(td.prices[i], td.volumes[i])) ++cnt;
            }
            fptr_count = cnt;
        });

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        double jit_vs_inline = static_cast<double>(cpp_exec_us) /
                               static_cast<double>(jit_exec_us);
        double jit_vs_fptr = static_cast<double>(fptr_exec_us) /
                             static_cast<double>(jit_exec_us);

        std::cout << "[JIT]  filter " << std::setw(4) << rows_str << " rows:"
                  << "  compile=" << std::setw(6) << compile_us << "μs"
                  << "  jit=" << std::setw(6) << jit_exec_us << "μs"
                  << "  inline=" << std::setw(6) << cpp_exec_us << "μs"
                  << "  fptr=" << std::setw(6) << fptr_exec_us << "μs"
                  << "  jit/inline=" << std::fixed << std::setprecision(2) << jit_vs_inline << "x"
                  << "  jit/fptr=" << std::fixed << std::setprecision(2) << jit_vs_fptr << "x"
                  << "  (match=" << (jit_count == cpp_count && cpp_count == fptr_count ? "YES" : "NO") << ")\n";
        (void)jit_count; (void)cpp_count; (void)fptr_count;
    }

    // ============================================================
    // Part 3: Phase B v2 최적화 변형 비교
    // ============================================================
    std::cout << "\n=============================================================\n";
    std::cout << " Phase B v2: Optimization Variant Comparison\n";
    std::cout << "=============================================================\n\n";

    std::cout << "--- [OPT] filter_gt_i64: SelectionVector(v1) vs BitMask(v2) ---\n";
    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        // v1: SelectionVector
        SelectionVector sel(rows);
        volatile size_t sel_count = 0;
        int64_t v1_us = time_us([&]{
            filter_gt_i64(td.prices.data(), rows, FILTER_THRESHOLD, sel);
            sel_count = sel.size();
        });

        // v2: BitMask
        BitMask bm(rows);
        volatile size_t bm_count = 0;
        int64_t v2_us = time_us([&]{
            filter_gt_i64_bitmask(td.prices.data(), rows, FILTER_THRESHOLD, bm);
            bm_count = bm.popcount();
        });

        double speedup = static_cast<double>(v1_us) / static_cast<double>(v2_us);
        bool match = (sel_count == bm_count);

        std::cout << "[OPT] filter_gt_i64 " << std::setw(4) << rows_str << ":"
                  << "  v1(SelVec)=" << std::setw(6) << v1_us << "μs"
                  << "  v2(BitMask)=" << std::setw(6) << v2_us << "μs"
                  << "  speedup=" << std::fixed << std::setprecision(2) << speedup << "x"
                  << "  (match=" << (match ? "YES" : "NO") << ", count=" << bm_count << ")\n";
        (void)sel_count; (void)bm_count;
    }

    std::cout << "\n--- [OPT] sum_i64: scalar → SIMD v1(4x) → fast(scalar) → SIMD v2(8x+pf) ---\n";
    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        volatile int64_t chk = 0;

        int64_t sc_us = time_us([&]{
            chk = scalar_sum(td.prices.data(), rows);
        });
        int64_t v1_us = time_us([&]{
            chk = sum_i64(td.prices.data(), rows);
        });
        int64_t fast_us = time_us([&]{
            chk = sum_i64_fast(td.prices.data(), rows);
        });
        int64_t v2_us = time_us([&]{
            chk = sum_i64_simd_v2(td.prices.data(), rows);
        });
        (void)chk;

        double v1_sp   = static_cast<double>(sc_us) / static_cast<double>(v1_us);
        double fast_sp = static_cast<double>(sc_us) / static_cast<double>(fast_us);
        double v2_sp   = static_cast<double>(sc_us) / static_cast<double>(v2_us);

        std::cout << "[OPT] sum_i64 " << std::setw(4) << rows_str << ":"
                  << "  scalar=" << std::setw(5) << sc_us << "μs"
                  << "  simd_v1=" << std::setw(5) << v1_us << "μs(" << std::fixed << std::setprecision(1) << v1_sp << "x)"
                  << "  fast=" << std::setw(5) << fast_us << "μs(" << std::fixed << std::setprecision(1) << fast_sp << "x)"
                  << "  simd_v2=" << std::setw(5) << v2_us << "μs(" << std::fixed << std::setprecision(1) << v2_sp << "x)\n";
    }

    std::cout << "\n--- [OPT] vwap: SIMD v1(2x) vs fused(4x+pf) ---\n";
    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        volatile double chk = 0.0;

        int64_t sc_us = time_us([&]{
            chk = scalar_vwap(td.prices.data(), td.volumes.data(), rows);
        });
        int64_t v1_us = time_us([&]{
            chk = vwap(td.prices.data(), td.volumes.data(), rows);
        });
        int64_t fused_us = time_us([&]{
            chk = vwap_fused(td.prices.data(), td.volumes.data(), rows);
        });
        (void)chk;

        double v1_sp    = static_cast<double>(sc_us) / static_cast<double>(v1_us);
        double fused_sp = static_cast<double>(sc_us) / static_cast<double>(fused_us);

        std::cout << "[OPT] vwap " << std::setw(4) << rows_str << ":"
                  << "  scalar=" << std::setw(5) << sc_us << "μs"
                  << "  v1(2x)=" << std::setw(5) << v1_us << "μs(" << std::fixed << std::setprecision(1) << v1_sp << "x)"
                  << "  fused(4x+pf)=" << std::setw(5) << fused_us << "μs(" << std::fixed << std::setprecision(1) << fused_sp << "x)\n";
    }

    // --------------------------------------------------------
    // JIT v2: bulk filter 벤치마크
    // --------------------------------------------------------
    std::cout << "\n--- [OPT] JIT filter: per-row(v1,O3) vs bulk(v2,O3 loop IR) ---\n";

    // compile_bulk
    auto compile_bulk_start = Clock::now();
    BulkFilterFn bulk_fn = jit.compile_bulk(expr);
    auto compile_bulk_end = Clock::now();

    if (!bulk_fn) {
        std::cerr << "[JIT] bulk 컴파일 실패: " << jit.last_error() << "\n";
        return 1;
    }

    int64_t compile_bulk_us = std::chrono::duration_cast<std::chrono::microseconds>(
        compile_bulk_end - compile_bulk_start).count();

    std::cout << "[JIT] Bulk compile time (O3 loop IR): " << compile_bulk_us << "μs\n";

    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        std::vector<uint32_t> idx_buf(rows);

        // JIT v1: per-row 호출
        volatile size_t v1_cnt = 0;
        int64_t v1_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (filter_fn(td.prices[i], td.volumes[i])) ++cnt;
            }
            v1_cnt = cnt;
        });

        // JIT v2: bulk 호출
        volatile size_t v2_cnt_val = 0;
        int64_t v2_us = time_us([&]{
            size_t cnt = 0;
            bulk_fn(td.prices.data(), td.volumes.data(), static_cast<size_t>(rows),
                    idx_buf.data(), &cnt);
            v2_cnt_val = cnt;
        });

        // C++ 함수포인터 기준
        FilterFn cpp_fptr2 = +[](int64_t price, int64_t volume) -> bool {
            return price > 100000 && volume > 5000;
        };
        volatile size_t fptr_cnt = 0;
        int64_t fptr_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (cpp_fptr2(td.prices[i], td.volumes[i])) ++cnt;
            }
            fptr_cnt = cnt;
        });

        bool match = (v1_cnt == v2_cnt_val && v1_cnt == fptr_cnt);
        double bulk_vs_v1   = static_cast<double>(v1_us)   / static_cast<double>(v2_us);
        double bulk_vs_fptr = static_cast<double>(fptr_us) / static_cast<double>(v2_us);

        std::cout << "[OPT] jit_filter " << std::setw(4) << rows_str << ":"
                  << "  per-row(O3)=" << std::setw(6) << v1_us << "μs"
                  << "  bulk(O3)=" << std::setw(6) << v2_us << "μs"
                  << "  fptr=" << std::setw(6) << fptr_us << "μs"
                  << "  bulk/v1=" << std::fixed << std::setprecision(2) << bulk_vs_v1 << "x"
                  << "  bulk/fptr=" << std::fixed << std::setprecision(2) << bulk_vs_fptr << "x"
                  << "  (match=" << (match ? "YES" : "NO") << ")\n";
        (void)v1_cnt; (void)v2_cnt_val; (void)fptr_cnt;
    }

    // --------------------------------------------------------
    // p50/p99 상세 측정 (1M rows)
    // --------------------------------------------------------
    std::cout << "\n--- [OPT] p50/p99 Latency @ 1M rows ---\n";
    {
        TestData td(1'000'000);
        SelectionVector sel(1'000'000);
        BitMask bm(1'000'000);

        auto [f_v1_p50, f_v1_p99] = time_percentiles([&]{
            filter_gt_i64(td.prices.data(), 1'000'000, FILTER_THRESHOLD, sel);
        });
        auto [f_v2_p50, f_v2_p99] = time_percentiles([&]{
            filter_gt_i64_bitmask(td.prices.data(), 1'000'000, FILTER_THRESHOLD, bm);
        });
        auto [s_v1_p50, s_v1_p99] = time_percentiles([&]{
            sum_i64(td.prices.data(), 1'000'000);
        });
        auto [s_v2_p50, s_v2_p99] = time_percentiles([&]{
            sum_i64_simd_v2(td.prices.data(), 1'000'000);
        });
        auto [s_fast_p50, s_fast_p99] = time_percentiles([&]{
            sum_i64_fast(td.prices.data(), 1'000'000);
        });
        auto [vwap_v1_p50, vwap_v1_p99] = time_percentiles([&]{
            vwap(td.prices.data(), td.volumes.data(), 1'000'000);
        });
        auto [vwap_v2_p50, vwap_v2_p99] = time_percentiles([&]{
            vwap_fused(td.prices.data(), td.volumes.data(), 1'000'000);
        });

        std::cout << std::left;
        std::cout << "[p50/p99] filter_v1(SelVec): p50=" << f_v1_p50 << "μs  p99=" << f_v1_p99 << "μs\n";
        std::cout << "[p50/p99] filter_v2(BitMask): p50=" << f_v2_p50 << "μs  p99=" << f_v2_p99 << "μs\n";
        std::cout << "[p50/p99] sum_v1(SIMD 4x):   p50=" << s_v1_p50 << "μs  p99=" << s_v1_p99 << "μs\n";
        std::cout << "[p50/p99] sum_fast(scalar):  p50=" << s_fast_p50 << "μs  p99=" << s_fast_p99 << "μs\n";
        std::cout << "[p50/p99] sum_v2(SIMD 8x+pf):p50=" << s_v2_p50 << "μs  p99=" << s_v2_p99 << "μs\n";
        std::cout << "[p50/p99] vwap_v1(2x):       p50=" << vwap_v1_p50 << "μs  p99=" << vwap_v1_p99 << "μs\n";
        std::cout << "[p50/p99] vwap_fused(4x+pf): p50=" << vwap_v2_p50 << "μs  p99=" << vwap_v2_p99 << "μs\n";
    }

    std::cout << "\n=============================================================\n";
    std::cout << " Benchmark Complete\n";
    std::cout << "=============================================================\n";
    return 0;
}

// ============================================================================
// Benchmark: SIMD vs Scalar vs JIT
// ============================================================================
// Phase B: Highway SIMD + LLVM JIT 성능 비교
// 측정 항목:
//   1. sum_i64:      scalar vs SIMD @ 100K/1M/10M rows
//   2. filter_gt:    scalar vs SIMD @ 100K/1M/10M rows
//   3. vwap:         scalar vs SIMD @ 100K/1M/10M rows
//   4. JIT filter:   compile time + execution vs C++ lambda
// ============================================================================

#include "apex/execution/vectorized_engine.h"
#include "apex/execution/jit_engine.h"

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

using namespace apex::execution;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 타이밍 헬퍼
// ============================================================================
template<typename Fn>
int64_t time_us(Fn&& fn, int warmup = 2, int iters = 5) {
    // 워밍업
    for (int i = 0; i < warmup; ++i) fn();

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto t1 = Clock::now();

    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / iters;
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
// 컴파일러 자동 벡터화 방지를 위해 noinline + pragma
// 순수 scalar 성능을 측정해야 SIMD 효과를 정확히 비교할 수 있음

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
    std::cout << " APEX-DB Phase B: SIMD + JIT Benchmark\n";
    std::cout << "=============================================================\n\n";

    // --------------------------------------------------------
    // Part 1: SIMD vs Scalar
    // --------------------------------------------------------
    std::cout << "--- [Part 1] SIMD vs Scalar ---\n";

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
    // Part 2: JIT Engine
    // --------------------------------------------------------
    std::cout << "--- [Part 2] JIT Compiled Filter ---\n";

    JITEngine jit;
    if (!jit.initialize()) {
        std::cerr << "[JIT] 초기화 실패: " << jit.last_error() << "\n";
        return 1;
    }

    // 테스트 표현식
    const std::string expr = "price > 100000 AND volume > 5000";

    // JIT 컴파일 시간 측정
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
    std::cout << "[JIT] Compile time: " << compile_us << "μs\n\n";

    // 동일한 조건의 C++ 람다
    auto cpp_lambda = [](int64_t price, int64_t volume) -> bool {
        return price > 100000 && volume > 5000;
    };

    for (size_t rows : {100'000UL, 1'000'000UL, 10'000'000UL}) {
        TestData td(rows);

        // JIT 실행 시간
        volatile size_t jit_count = 0;
        int64_t jit_exec_us = time_us([&]{
            auto r = jit.apply(filter_fn, td.prices.data(), td.volumes.data(), rows);
            jit_count = r.size();
        });

        // C++ 람다 실행 시간
        volatile size_t cpp_count = 0;
        int64_t cpp_exec_us = time_us([&]{
            size_t cnt = 0;
            for (size_t i = 0; i < rows; ++i) {
                if (cpp_lambda(td.prices[i], td.volumes[i])) ++cnt;
            }
            cpp_count = cnt;
        });

        std::string rows_str;
        if (rows >= 1'000'000) rows_str = std::to_string(rows / 1'000'000) + "M";
        else rows_str = std::to_string(rows / 1'000) + "K";

        double ratio = static_cast<double>(cpp_exec_us) /
                       static_cast<double>(jit_exec_us);

        std::cout << "[JIT]  filter " << std::setw(4) << rows_str << " rows:"
                  << "  compile=" << std::setw(6) << compile_us << "μs"
                  << "  jit_exec=" << std::setw(6) << jit_exec_us << "μs"
                  << "  cpp_lambda=" << std::setw(6) << cpp_exec_us << "μs"
                  << "  ratio=" << std::fixed << std::setprecision(2) << ratio << "x"
                  << "  (matched=" << (jit_count == cpp_count ? "YES" : "NO") << ")\n";
        (void)jit_count; (void)cpp_count;
    }

    std::cout << "\n=============================================================\n";
    std::cout << " Benchmark Complete\n";
    std::cout << "=============================================================\n";
    return 0;
}

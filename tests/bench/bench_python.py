#!/usr/bin/env python3
"""
tests/bench/bench_python.py — ZeptoDB Python 바인딩 벤치마크

비교 대상:
  - Polars (Rust 기반 DataFrame, Lazy API)
  - ZeptoDB C++ 벡터화 엔진

측정 항목:
  1. VWAP 계산 (N=100K 틱)
  2. Filter + Sum (price > threshold)
  3. COUNT
  4. Zero-copy get_column vs Polars eager column access

실행:
    cd ~/zeptodb
    python3 tests/bench/bench_python.py
"""

import sys
import os
import time
import statistics

import numpy as np

BUILD_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "build")
sys.path.insert(0, os.path.abspath(BUILD_DIR))

import zeptodb  # apex.so (pybind11)

# DSL import
sys.path.insert(0, os.path.abspath(BUILD_DIR))
from zepto_py.dsl import DataFrame as ZeptoDF

try:
    import polars as pl
    POLARS_AVAILABLE = True
except ImportError:
    print("[WARN] polars not installed. Run: pip3 install polars")
    POLARS_AVAILABLE = False

# ============================================================================
# 벤치마크 설정
# ============================================================================
SYMBOL      = 1
N_TICKS     = 100_000
N_WARMUP    = 3
N_RUNS      = 10
BASE_PRICE  = 10_000
THRESHOLD   = 10_500  # filter: price > THRESHOLD

SEPARATOR = "─" * 64


def bench_fn(fn, n_warmup=N_WARMUP, n_runs=N_RUNS):
    """함수 fn을 n_runs번 실행해 latency 통계 반환 (ns)."""
    for _ in range(n_warmup):
        fn()
    times = []
    for _ in range(n_runs):
        t0 = time.perf_counter_ns()
        result = fn()
        t1 = time.perf_counter_ns()
        times.append(t1 - t0)
    return times, result


def fmt_ns(ns: float) -> str:
    if ns < 1_000:
        return f"{ns:.0f}ns"
    elif ns < 1_000_000:
        return f"{ns/1_000:.1f}μs"
    else:
        return f"{ns/1_000_000:.2f}ms"


def print_stats(label: str, times_ns: list):
    med = statistics.median(times_ns)
    mn  = min(times_ns)
    mx  = max(times_ns)
    print(f"  {label:<28} median={fmt_ns(med)}  min={fmt_ns(mn)}  max={fmt_ns(mx)}")


def print_speedup(label_a: str, a_ns: list, label_b: str, b_ns: list):
    speedup = statistics.median(a_ns) / statistics.median(b_ns)
    winner = label_b if speedup > 1 else label_a
    print(f"  Speedup ({winner} wins): {max(speedup, 1/speedup):.1f}x")


# ============================================================================
# 데이터 준비
# ============================================================================

def setup_zeptodb():
    print(f"[Setup] ZeptoDB 파이프라인 시작 + {N_TICKS:,} 틱 인제스트...")
    db = zeptodb.Pipeline()
    db.start()

    syms   = np.full(N_TICKS, SYMBOL, dtype=np.int64)
    prices = np.arange(BASE_PRICE, BASE_PRICE + N_TICKS, dtype=np.int64)
    vols   = np.arange(1, N_TICKS + 1, dtype=np.int64)

    # RingBuffer 크기(65536) 이하로 청크 분할 인제스트
    CHUNK = 50_000
    t0 = time.perf_counter_ns()
    for start in range(0, N_TICKS, CHUNK):
        end = min(start + CHUNK, N_TICKS)
        db.ingest_batch(
            symbols=syms[start:end],
            prices=prices[start:end],
            volumes=vols[start:end],
        )
        db.drain()  # 청크마다 drain해서 큐 비우기
    t1 = time.perf_counter_ns()

    ingest_ms = (t1 - t0) / 1e6
    stored = db.stats()["ticks_stored"]
    print(f"[Setup] 인제스트 완료: {stored:,} 틱 저장, {ingest_ms:.1f}ms")
    return db, prices, vols


def setup_polars(prices_np, vols_np):
    if not POLARS_AVAILABLE:
        return None
    df = pl.DataFrame({
        "price":  prices_np.astype("int64"),
        "volume": vols_np.astype("int64"),
    })
    return df


# ============================================================================
# 벤치마크 1: VWAP
# ============================================================================

def bench_vwap(db, polars_df, prices_np, vols_np):
    print(f"\n{'='*64}")
    print(f"  벤치마크 1: VWAP  (N={N_TICKS:,} rows)")
    print(SEPARATOR)

    # ZEPTO VWAP
    zepto_times, zepto_result = bench_fn(lambda: db.vwap(symbol=SYMBOL))
    print_stats("ZEPTO VWAP (C++ vectorized)", zepto_times)
    print(f"    → result: {zepto_result.value:.4f}")

    if POLARS_AVAILABLE and polars_df is not None:
        # Polars Lazy VWAP
        def polars_lazy_vwap():
            return (
                polars_df.lazy()
                .select([
                    (pl.col("price") * pl.col("volume")).sum().alias("pv_sum"),
                    pl.col("volume").sum().alias("vol_sum"),
                ])
                .collect()
                .row(0)
            )

        pol_times, pol_result = bench_fn(polars_lazy_vwap)
        pv_sum, vol_sum = pol_result
        polars_vwap = pv_sum / vol_sum
        print_stats("Polars Lazy VWAP (Rust)", pol_times)
        print(f"    → result: {polars_vwap:.4f}")
        print_speedup("Polars", pol_times, "ZEPTO", zepto_times)

        # Polars Eager VWAP (비교용)
        def polars_eager_vwap():
            return (polars_df["price"] * polars_df["volume"]).sum() / polars_df["volume"].sum()

        pol_eager_times, _ = bench_fn(polars_eager_vwap)
        print_stats("Polars Eager VWAP (Rust)", pol_eager_times)
    else:
        # NumPy fallback 비교
        def numpy_vwap():
            return np.sum(prices_np * vols_np) / np.sum(vols_np)

        np_times, np_result = bench_fn(numpy_vwap)
        print_stats("NumPy VWAP (fallback)", np_times)
        print(f"    → result: {np_result:.4f}")
        print_speedup("NumPy", np_times, "ZEPTO", zepto_times)


# ============================================================================
# 벤치마크 2: Filter + Sum
# ============================================================================

def bench_filter_sum(db, polars_df, prices_np, vols_np):
    print(f"\n{'='*64}")
    print(f"  벤치마크 2: Filter + Sum  (price > {THRESHOLD:,})")
    print(SEPARATOR)

    # ZEPTO filter_sum
    zepto_times, zepto_result = bench_fn(
        lambda: db.filter_sum(symbol=SYMBOL, column="price", threshold=THRESHOLD)
    )
    print_stats("ZEPTO filter_sum (C++ SIMD)", zepto_times)
    print(f"    → result: {zepto_result.ivalue:,}")

    if POLARS_AVAILABLE and polars_df is not None:
        # Polars Lazy filter+sum
        def polars_lazy_filter_sum():
            return (
                polars_df.lazy()
                .filter(pl.col("price") > THRESHOLD)
                .select(pl.col("price").sum())
                .collect()
                .item()
            )

        pol_times, pol_result = bench_fn(polars_lazy_filter_sum)
        print_stats("Polars Lazy filter+sum (Rust)", pol_times)
        print(f"    → result: {pol_result:,}")
        print_speedup("Polars", pol_times, "ZEPTO", zepto_times)
    else:
        def numpy_filter_sum():
            mask = prices_np > THRESHOLD
            return int(np.sum(prices_np[mask]))

        np_times, np_result = bench_fn(numpy_filter_sum)
        print_stats("NumPy filter+sum (fallback)", np_times)
        print(f"    → result: {np_result:,}")
        print_speedup("NumPy", np_times, "ZEPTO", zepto_times)


# ============================================================================
# 벤치마크 3: COUNT
# ============================================================================

def bench_count(db, polars_df):
    print(f"\n{'='*64}")
    print(f"  벤치마크 3: COUNT (full scan)")
    print(SEPARATOR)

    zepto_times, zepto_result = bench_fn(lambda: db.count(symbol=SYMBOL))
    print_stats("APEX count (C++ vectorized)", zepto_times)
    print(f"    → result: {zepto_result.ivalue:,}")

    if POLARS_AVAILABLE and polars_df is not None:
        pol_times, pol_result = bench_fn(
            lambda: polars_df.lazy().select(pl.count()).collect().item()
        )
        print_stats("Polars count (Rust)", pol_times)
        print(f"    → result: {pol_result:,}")
        print_speedup("Polars", pol_times, "ZEPTO", zepto_times)


# ============================================================================
# 벤치마크 4: Zero-copy vs Polars column access
# ============================================================================

def bench_zero_copy(db, polars_df):
    print(f"\n{'='*64}")
    print(f"  벤치마크 4: Zero-copy Column Access")
    print(SEPARATOR)
    print(f"  (numpy array 포인터만 반환 vs Polars Series)")

    # APEX get_column (zero-copy)
    zepto_times, zepto_arr = bench_fn(
        lambda: db.get_column(symbol=SYMBOL, name="price")
    )
    print_stats("APEX get_column (zero-copy)", zepto_times)
    print(f"    → len={len(zepto_arr)}, ptr_owned={zepto_arr.flags['OWNDATA']}")

    if POLARS_AVAILABLE and polars_df is not None:
        # Polars series access (내부적으로 Arrow)
        pol_times, _ = bench_fn(lambda: polars_df["price"])
        print_stats("Polars Series access (Rust)", pol_times)
        print_speedup("Polars", pol_times, "ZEPTO", zepto_times)


# ============================================================================
# 벤치마크 5: DSL lazy chain
# ============================================================================

def bench_dsl(db, polars_df):
    print(f"\n{'='*64}")
    print(f"  벤치마크 5: Lazy DSL — filter + sum chain")
    print(SEPARATOR)

    zepto_df = ZeptoDF(db, symbol=SYMBOL)

    # APEX DSL lazy chain
    def zepto_dsl_filter_sum():
        lazy = zepto_df[zepto_df['price'] > THRESHOLD]['price'].sum()
        return lazy.collect()

    zepto_times, zepto_val = bench_fn(zepto_dsl_filter_sum)
    print_stats("APEX DSL lazy chain", zepto_times)
    print(f"    → result: {zepto_val:,}")

    if POLARS_AVAILABLE and polars_df is not None:
        def polars_lazy_chain():
            return (
                polars_df.lazy()
                .filter(pl.col("price") > THRESHOLD)
                .select(pl.col("price").sum())
                .collect()
                .item()
            )

        pol_times, pol_val = bench_fn(polars_lazy_chain)
        print_stats("Polars .lazy().filter().sum() (Rust)", pol_times)
        print(f"    → result: {pol_val:,}")
        print_speedup("Polars", pol_times, "ZEPTO", zepto_times)


# ============================================================================
# 인제스트 처리량 벤치마크
# ============================================================================

def bench_ingest_throughput():
    print(f"\n{'='*64}")
    print(f"  벤치마크 6: 인제스트 처리량 (단건 / batch)")
    print(SEPARATOR)
    print("  ※ drain() 시간 제외, 순수 ingest 호출 비용만 측정")

    # 단건 ingest 비용
    db = zeptodb.Pipeline()
    db.start()
    N = 10_000
    t0 = time.perf_counter_ns()
    for i in range(N):
        db.ingest(symbol=88, price=10000 + i, volume=i + 1)
    t1 = time.perf_counter_ns()
    db.drain()
    db.stop()

    per_tick_ns = (t1 - t0) / N
    tps = 1e9 / per_tick_ns
    print(f"  단건 ingest x{N:,}: {fmt_ns(per_tick_ns)}/tick  "
          f"→ {tps/1_000_000:.1f}M ticks/s")

    # 배치 ingest 비용 (큐 크기 이하)
    BATCH_SIZES = [1_000, 5_000, 10_000, 30_000]
    for bs in BATCH_SIZES:
        syms   = np.full(bs, 77, dtype=np.int64)
        prices = np.arange(10000, 10000 + bs, dtype=np.int64)
        vols   = np.ones(bs, dtype=np.int64)

        db = zeptodb.Pipeline()
        db.start()

        times = []
        for _ in range(5):
            t0 = time.perf_counter_ns()
            db.ingest_batch(symbols=syms, prices=prices, volumes=vols)
            t1 = time.perf_counter_ns()
            times.append(t1 - t0)
        db.drain()
        db.stop()

        med_ns = statistics.median(times)
        tps = bs / (med_ns / 1e9)
        print(f"  batch={bs:>6,}  median={fmt_ns(med_ns)}  "
              f"→ {tps/1_000_000:.1f}M ticks/s  ({fmt_ns(med_ns/bs)}/tick)")


# ============================================================================
# Main
# ============================================================================

def main():
    print()
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║         ZeptoDB Python Bridge Benchmark                     ║")
    print(f"║  N={N_TICKS:,} ticks, {N_RUNS} runs, {N_WARMUP} warmup                         ║")
    if POLARS_AVAILABLE:
        import polars
        print(f"║  Polars v{polars.__version__} (Rust) vs ZeptoDB C++ SIMD           ║")
    print("╚══════════════════════════════════════════════════════════════╝")

    # 데이터 준비
    db, prices_np, vols_np = setup_zeptodb()
    polars_df = setup_polars(prices_np, vols_np)

    if POLARS_AVAILABLE:
        print(f"\n[Setup] Polars DataFrame: {polars_df.shape} (in-memory)")

    # 벤치마크 실행
    bench_vwap(db, polars_df, prices_np, vols_np)
    bench_filter_sum(db, polars_df, prices_np, vols_np)
    bench_count(db, polars_df)
    bench_zero_copy(db, polars_df)
    bench_dsl(db, polars_df)
    bench_ingest_throughput()

    db.stop()

    print(f"\n{'='*64}")
    print("  벤치마크 완료")
    print(f"  ✓ Zero-copy get_column: numpy array가 RDB 메모리 직접 참조")
    if POLARS_AVAILABLE:
        print("  ✓ Polars (Rust) 와 직접 비교 완료")
    print(f"{'='*64}\n")


if __name__ == "__main__":
    main()

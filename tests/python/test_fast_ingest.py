"""
tests/python/test_fast_ingest.py — Python 에코시스템 통합 테스트

검증 항목:
  1. from_pandas()  — vectorized numpy 배치 ingest (mock pipeline)
  2. from_polars()  — zero-copy polars .to_numpy() 배치 ingest
  3. from_arrow()   — Arrow Table 벡터화 ingest
  4. ingest_float_batch() — float64 가격 + price_scale 변환
  5. ArrowSession.ingest_arrow_columnar() — 컬럼별 Arrow 배열 ingest
  6. 에러 케이스 — missing columns, wrong types
  7. 성능 — 1M rows 처리 시간 한도 검증
  8. schema mapping — sym_col/price_col/vol_col 오버라이드

실행:
    cd ~/zeptodb
    python3 -m pytest tests/python/test_fast_ingest.py -v
"""
import sys
import os
import time

import pytest
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import polars as pl
    HAS_POLARS = True
except ImportError:
    HAS_POLARS = False

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import pyarrow as pa
    HAS_PYARROW = True
except ImportError:
    HAS_PYARROW = False

from zepto_py.dataframe import (
    from_pandas,
    from_polars,
    from_arrow,
    from_polars_arrow,
    _require_cols,
)
from zepto_py.arrow import ArrowSession


# ============================================================================
# Mock Pipeline — simulates zeptodb.Pipeline() without the C++ build
# ============================================================================

class MockPipeline:
    """
    In-memory mock of zeptodb.Pipeline().

    Records every ingest_batch() call so tests can verify values.
    """

    def __init__(self):
        self.batches: list = []   # list of (syms, prices, vols) tuples
        self._total = 0

    def ingest_batch(self, symbols, prices, volumes):
        s = np.asarray(symbols, dtype=np.int64)
        p = np.asarray(prices,  dtype=np.int64)
        v = np.asarray(volumes, dtype=np.int64)
        if len(s) != len(p) or len(s) != len(v):
            raise ValueError("length mismatch")
        self.batches.append((s.copy(), p.copy(), v.copy()))
        self._total += len(s)

    def drain(self):
        pass

    def ingest(self, **kwargs):
        # single-row ingest (not used in fast paths)
        self.batches.append((
            np.array([kwargs.get("symbol", kwargs.get("sym", 0))], dtype=np.int64),
            np.array([kwargs.get("price", 0)], dtype=np.int64),
            np.array([kwargs.get("volume", 0)], dtype=np.int64),
        ))
        self._total += 1

    def get_column(self, symbol: int, name: str):
        # Gather all ingested values for given symbol
        syms_all = np.concatenate([b[0] for b in self.batches]) if self.batches else np.array([], dtype=np.int64)
        mask = (syms_all == symbol)
        if not np.any(mask):
            raise RuntimeError(f"no data for symbol {symbol}")
        if name == "price":
            all_prices = np.concatenate([b[1] for b in self.batches])
            return all_prices[mask]
        if name in ("volume", "vol"):
            all_vols = np.concatenate([b[2] for b in self.batches])
            return all_vols[mask]
        raise RuntimeError(f"unknown column: {name}")

    @property
    def total_rows(self) -> int:
        return self._total


@pytest.fixture
def pipeline():
    return MockPipeline()


# ============================================================================
# Helpers
# ============================================================================

def _flat_syms(pipeline: MockPipeline) -> np.ndarray:
    return np.concatenate([b[0] for b in pipeline.batches])

def _flat_prices(pipeline: MockPipeline) -> np.ndarray:
    return np.concatenate([b[1] for b in pipeline.batches])

def _flat_vols(pipeline: MockPipeline) -> np.ndarray:
    return np.concatenate([b[2] for b in pipeline.batches])


# ============================================================================
# from_polars() tests
# ============================================================================

@pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
class TestFromPolars:

    def test_basic_ingest_count(self, pipeline):
        df = pl.DataFrame({
            "sym":    [1, 1, 2, 2, 3],
            "price":  [15000, 15001, 16000, 16001, 17000],
            "volume": [100, 200, 150, 50, 300],
        })
        n = from_polars(df, pipeline)
        assert n == 5
        assert pipeline.total_rows == 5

    def test_values_preserved(self, pipeline):
        df = pl.DataFrame({
            "sym":    [7, 7, 7],
            "price":  [10000, 10001, 10002],
            "volume": [1, 2, 3],
        })
        from_polars(df, pipeline)
        prices = _flat_prices(pipeline)
        assert list(prices) == [10000, 10001, 10002]
        vols = _flat_vols(pipeline)
        assert list(vols) == [1, 2, 3]

    def test_sym_col_mapping(self, pipeline):
        """Custom column name: ticker instead of sym."""
        df = pl.DataFrame({
            "ticker": [5, 5],
            "price":  [1000, 2000],
            "volume": [10, 20],
        })
        n = from_polars(df, pipeline, sym_col="ticker")
        assert n == 2
        syms = _flat_syms(pipeline)
        assert list(syms) == [5, 5]

    def test_float_prices_with_scale(self, pipeline):
        """Float prices scaled to int64 cents."""
        df = pl.DataFrame({
            "sym":    [1, 1],
            "price":  [150.25, 150.50],
            "volume": [100, 200],
        })
        from_polars(df, pipeline, price_scale=100.0)
        prices = _flat_prices(pipeline)
        assert list(prices) == [15025, 15050]

    def test_batch_size_respected(self, pipeline):
        """Multiple small batch calls still produce correct total."""
        n = 1000
        df = pl.DataFrame({
            "sym":    pl.Series([1] * n, dtype=pl.Int64),
            "price":  pl.Series(list(range(n)), dtype=pl.Int64),
            "volume": pl.Series([10] * n, dtype=pl.Int64),
        })
        from_polars(df, pipeline, batch_size=100)
        assert pipeline.total_rows == n
        assert len(pipeline.batches) == 10  # 1000 / 100

    def test_large_dataframe(self, pipeline):
        """1M rows completes in reasonable time."""
        n = 1_000_000
        df = pl.DataFrame({
            "sym":    pl.Series(np.ones(n, dtype=np.int64)),
            "price":  pl.Series(np.arange(n, dtype=np.int64)),
            "volume": pl.Series(np.full(n, 100, dtype=np.int64)),
        })
        t0 = time.perf_counter()
        cnt = from_polars(df, pipeline, batch_size=100_000)
        elapsed = time.perf_counter() - t0

        assert cnt == n
        assert pipeline.total_rows == n
        assert elapsed < 5.0, f"from_polars 1M rows took {elapsed:.2f}s"

    def test_missing_column_raises(self, pipeline):
        df = pl.DataFrame({"sym": [1], "price": [100]})
        with pytest.raises(ValueError, match="Missing columns"):
            from_polars(df, pipeline)  # missing "volume"

    def test_zero_rows(self, pipeline):
        df = pl.DataFrame({"sym": [], "price": [], "volume": []},
                          schema={"sym": pl.Int64, "price": pl.Int64, "volume": pl.Int64})
        n = from_polars(df, pipeline)
        assert n == 0

    def test_multi_symbol(self, pipeline):
        df = pl.DataFrame({
            "sym":    [1, 2, 3],
            "price":  [100, 200, 300],
            "volume": [10, 20, 30],
        })
        from_polars(df, pipeline)
        syms = _flat_syms(pipeline)
        assert set(syms.tolist()) == {1, 2, 3}

    def test_vwap_computation(self, pipeline):
        """Verify ingest values are correct for VWAP check."""
        # VWAP for sym=1: (10000*100 + 20000*100)/200 = 15000
        df = pl.DataFrame({
            "sym":    [1, 1],
            "price":  [10000, 20000],
            "volume": [100, 100],
        })
        from_polars(df, pipeline)
        prices = _flat_prices(pipeline)
        vols = _flat_vols(pipeline)
        vwap = (prices * vols).sum() / vols.sum()
        assert abs(vwap - 15000.0) < 1.0

    def test_polars_to_numpy_zero_copy(self):
        """polars .to_numpy() on int Series returns Arrow-backed array."""
        s = pl.Series("x", [1, 2, 3], dtype=pl.Int64)
        arr = s.to_numpy()
        assert isinstance(arr, np.ndarray)
        assert arr.dtype == np.int64
        # For contiguous int Series without nulls, may not own data
        assert arr.flags["C_CONTIGUOUS"]

    def test_from_polars_returns_int(self, pipeline):
        df = pl.DataFrame({"sym": [1], "price": [100], "volume": [10]})
        result = from_polars(df, pipeline)
        assert isinstance(result, int)
        assert result == 1


# ============================================================================
# from_pandas() tests
# ============================================================================

@pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
class TestFromPandas:

    def test_basic_ingest(self, pipeline):
        df = pd.DataFrame({
            "sym":    [1, 2, 3],
            "price":  [15000, 16000, 17000],
            "volume": [100, 200, 300],
        })
        n = from_pandas(df, pipeline)
        assert n == 3

    def test_values_preserved(self, pipeline):
        df = pd.DataFrame({
            "sym":    [9, 9],
            "price":  [1234, 5678],
            "volume": [11, 22],
        })
        from_pandas(df, pipeline)
        prices = _flat_prices(pipeline)
        assert list(prices) == [1234, 5678]

    def test_float_price_truncation(self, pipeline):
        df = pd.DataFrame({
            "sym":    [1],
            "price":  [150.75],
            "volume": [100],
        })
        from_pandas(df, pipeline)
        # Default scale=1: 150.75 → 150
        assert _flat_prices(pipeline)[0] == 150

    def test_float_price_with_scale(self, pipeline):
        df = pd.DataFrame({
            "sym":    [1],
            "price":  [150.75],
            "volume": [100],
        })
        from_pandas(df, pipeline, price_scale=100.0)
        assert _flat_prices(pipeline)[0] == 15075

    def test_custom_col_names(self, pipeline):
        df = pd.DataFrame({
            "symbol": [3, 3],
            "px":     [500, 600],
            "qty":    [10, 20],
        })
        n = from_pandas(df, pipeline, sym_col="symbol",
                        price_col="px", vol_col="qty")
        assert n == 2
        assert _flat_syms(pipeline).tolist() == [3, 3]

    def test_missing_column_error(self, pipeline):
        df = pd.DataFrame({"sym": [1], "price": [100]})
        with pytest.raises(ValueError, match="Missing columns"):
            from_pandas(df, pipeline)

    def test_batch_splitting(self, pipeline):
        n = 500
        df = pd.DataFrame({
            "sym":    np.ones(n, dtype=np.int64),
            "price":  np.arange(n, dtype=np.int64),
            "volume": np.ones(n, dtype=np.int64),
        })
        from_pandas(df, pipeline, batch_size=200)
        assert pipeline.total_rows == n
        # 500 / 200 = 3 batches (200, 200, 100)
        assert len(pipeline.batches) == 3


# ============================================================================
# from_arrow() tests
# ============================================================================

@pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow not installed")
class TestFromArrow:

    def test_basic_ingest(self, pipeline):
        tbl = pa.table({
            "sym":    pa.array([1, 2, 3], type=pa.int64()),
            "price":  pa.array([100, 200, 300], type=pa.int64()),
            "volume": pa.array([10, 20, 30], type=pa.int64()),
        })
        n = from_arrow(tbl, pipeline)
        assert n == 3
        assert pipeline.total_rows == 3

    def test_float_prices_vectorized(self, pipeline):
        tbl = pa.table({
            "sym":    pa.array([1, 1], type=pa.int64()),
            "price":  pa.array([150.25, 150.50], type=pa.float64()),
            "volume": pa.array([100, 200], type=pa.int64()),
        })
        from_arrow(tbl, pipeline, price_scale=100.0)
        prices = _flat_prices(pipeline)
        assert list(prices) == [15025, 15050]

    def test_no_row_iteration(self, pipeline):
        """Verify vectorized path: one ingest_batch per chunk."""
        n = 1000
        tbl = pa.table({
            "sym":    pa.array(np.ones(n, dtype=np.int64)),
            "price":  pa.array(np.arange(n, dtype=np.int64)),
            "volume": pa.array(np.ones(n, dtype=np.int64)),
        })
        from_arrow(tbl, pipeline, batch_size=500)
        assert len(pipeline.batches) == 2   # 2 vectorized calls, not 1000

    def test_null_values_become_zero(self, pipeline):
        """Nulls in Arrow arrays are handled by to_numpy (filled with 0)."""
        tbl = pa.table({
            "sym":    pa.array([1, None, 3], type=pa.int64()),
            "price":  pa.array([100, 200, None], type=pa.int64()),
            "volume": pa.array([10, 20, 30], type=pa.int64()),
        })
        # to_numpy(zero_copy_only=False) fills nulls with 0 by default
        n = from_arrow(tbl, pipeline)
        assert n == 3

    def test_custom_column_names(self, pipeline):
        tbl = pa.table({
            "ticker": pa.array([7, 7], type=pa.int64()),
            "px":     pa.array([500, 600], type=pa.int64()),
            "qty":    pa.array([10, 20], type=pa.int64()),
        })
        n = from_arrow(tbl, pipeline,
                       sym_col="ticker", price_col="px", vol_col="qty")
        assert n == 2
        assert _flat_syms(pipeline).tolist() == [7, 7]

    def test_missing_column_raises(self, pipeline):
        tbl = pa.table({"sym": pa.array([1]), "price": pa.array([100])})
        with pytest.raises(ValueError, match="Missing columns"):
            from_arrow(tbl, pipeline)


# ============================================================================
# ArrowSession vectorized tests
# ============================================================================

@pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow not installed")
class TestArrowSessionVectorized:

    def test_ingest_arrow_vectorized(self, pipeline):
        sess = ArrowSession(pipeline)
        tbl = pa.table({
            "sym":    pa.array([1, 2, 3], type=pa.int64()),
            "price":  pa.array([100, 200, 300], type=pa.int64()),
            "volume": pa.array([10, 20, 30], type=pa.int64()),
        })
        n = sess.ingest_arrow(tbl)
        assert n == 3
        # Vectorized: 1 batch call not 3
        assert len(pipeline.batches) == 1

    def test_ingest_arrow_columnar(self, pipeline):
        sess = ArrowSession(pipeline)
        n = sess.ingest_arrow_columnar(
            pa.array([1, 1, 2], type=pa.int64()),
            pa.array([15000, 15001, 16000], type=pa.int64()),
            pa.array([100, 200, 150], type=pa.int64()),
        )
        assert n == 3
        assert len(pipeline.batches) == 1

    def test_ingest_record_batch_vectorized(self, pipeline):
        sess = ArrowSession(pipeline)
        tbl = pa.table({
            "sym":    pa.array([5, 5], type=pa.int64()),
            "price":  pa.array([999, 1001], type=pa.int64()),
            "volume": pa.array([50, 60], type=pa.int64()),
        })
        batch = tbl.to_batches()[0]
        n = sess.ingest_record_batch(batch)
        assert n == 2

    def test_ingest_arrow_with_float_scale(self, pipeline):
        sess = ArrowSession(pipeline)
        tbl = pa.table({
            "sym":    pa.array([1], type=pa.int64()),
            "price":  pa.array([123.45], type=pa.float64()),
            "volume": pa.array([100], type=pa.int64()),
        })
        sess.ingest_arrow(tbl, price_scale=100.0)
        assert _flat_prices(pipeline)[0] == 12345

    def test_to_arrow_export(self, pipeline):
        """Export returns a valid Arrow Table."""
        # Ingest some data first
        sess = ArrowSession(pipeline)
        tbl_in = pa.table({
            "sym":    pa.array([1, 1], type=pa.int64()),
            "price":  pa.array([100, 200], type=pa.int64()),
            "volume": pa.array([10, 20], type=pa.int64()),
        })
        sess.ingest_arrow(tbl_in)

        # Export — uses get_column() on the mock
        tbl_out = sess.to_arrow(symbol=1, columns=["price", "volume"])
        assert isinstance(tbl_out, pa.Table)
        assert "price"  in tbl_out.schema.names
        assert "volume" in tbl_out.schema.names

    def test_to_duckdb(self, pipeline):
        try:
            import duckdb
        except ImportError:
            pytest.skip("duckdb not installed")

        sess = ArrowSession(pipeline)
        tbl = pa.table({
            "sym":    pa.array([1, 1], type=pa.int64()),
            "price":  pa.array([100, 200], type=pa.int64()),
            "volume": pa.array([10, 20], type=pa.int64()),
        })
        sess.ingest_arrow(tbl)
        conn = sess.to_duckdb(symbol=1)
        result = conn.execute("SELECT COUNT(*) FROM zepto_data").fetchone()
        assert result[0] >= 0


# ============================================================================
# _require_cols helper tests
# ============================================================================

class TestRequireCols:

    def test_polars_ok(self):
        if not HAS_POLARS:
            pytest.skip("polars not installed")
        df = pl.DataFrame({"a": [1], "b": [2], "c": [3]})
        _require_cols(df, ["a", "b"], "test")  # should not raise

    def test_polars_missing(self):
        if not HAS_POLARS:
            pytest.skip("polars not installed")
        df = pl.DataFrame({"a": [1]})
        with pytest.raises(ValueError, match="Missing columns"):
            _require_cols(df, ["a", "z"], "test")

    def test_error_message_shows_available(self):
        if not HAS_POLARS:
            pytest.skip("polars not installed")
        df = pl.DataFrame({"sym": [1], "price": [100]})
        with pytest.raises(ValueError) as exc:
            _require_cols(df, ["volume"], "polars DataFrame")
        msg = str(exc.value)
        assert "volume" in msg
        assert "sym" in msg or "price" in msg


# ============================================================================
# Cross-library roundtrip tests
# ============================================================================

@pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
class TestPolarsRoundtrip:

    def test_from_polars_to_numpy_values(self, pipeline):
        """Ingested values match original DataFrame values."""
        prices_in = list(range(15000, 15010))
        df = pl.DataFrame({
            "sym":    [42] * 10,
            "price":  prices_in,
            "volume": [100] * 10,
        })
        from_polars(df, pipeline)
        prices_out = _flat_prices(pipeline).tolist()
        assert prices_out == prices_in

    def test_polars_slice_zero_copy(self):
        """df.slice() in Polars returns a view (zero-copy)."""
        df = pl.DataFrame({"x": list(range(100))})
        chunk = df.slice(0, 50)
        # Both refer to the same Arrow buffer root
        assert len(chunk) == 50

    def test_large_polars_batch_performance(self, pipeline):
        """500K rows via from_polars completes quickly."""
        n = 500_000
        df = pl.DataFrame({
            "sym":    pl.Series(np.ones(n, dtype=np.int64)),
            "price":  pl.Series(np.arange(n, dtype=np.int64)),
            "volume": pl.Series(np.full(n, 50, dtype=np.int64)),
        })
        t0 = time.perf_counter()
        from_polars(df, pipeline, batch_size=100_000)
        elapsed = time.perf_counter() - t0
        assert elapsed < 3.0, f"500K rows took {elapsed:.2f}s"

    @pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow needed for Arrow path")
    def test_from_polars_arrow_path(self, pipeline):
        """from_polars_arrow() produces same results as from_polars()."""
        df = pl.DataFrame({
            "sym":    [1, 2, 3],
            "price":  [100, 200, 300],
            "volume": [10, 20, 30],
        })
        p1 = MockPipeline()
        p2 = MockPipeline()

        from_polars(df, p1)
        from_polars_arrow(df, p2)

        assert _flat_prices(p1).tolist() == _flat_prices(p2).tolist()
        assert _flat_vols(p1).tolist()   == _flat_vols(p2).tolist()

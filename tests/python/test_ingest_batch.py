"""
Tests for the vectorized ingest functions in zepto_py.dataframe:
  from_pandas(), from_polars(), from_polars_arrow(), from_arrow()

These functions use pipeline.ingest_batch(symbols, prices, volumes) instead of
row-by-row pipeline.ingest(**kwargs), so they need a different MockPipeline.
"""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import polars as pl
    HAS_POLARS = True
except ImportError:
    HAS_POLARS = False

try:
    import pyarrow as pa
    HAS_PYARROW = True
except ImportError:
    HAS_PYARROW = False

from zepto_py.dataframe import (
    from_pandas,
    from_polars,
    from_polars_arrow,
    from_arrow,
    _require_cols,
)


# ============================================================================
# Mock pipeline — ingest_batch API
# ============================================================================

class BatchPipeline:
    """Mock pipeline that records ingest_batch() calls."""

    def __init__(self):
        self.batches = []      # list of (symbols, prices, volumes) arrays
        self.drain_calls = 0

    def ingest_batch(self, symbols, prices, volumes):
        self.batches.append((
            list(symbols),
            list(prices),
            list(volumes),
        ))

    def drain(self):
        self.drain_calls += 1

    @property
    def total_rows(self):
        return sum(len(b[0]) for b in self.batches)

    @property
    def all_syms(self):
        return [v for b in self.batches for v in b[0]]

    @property
    def all_prices(self):
        return [v for b in self.batches for v in b[1]]

    @property
    def all_vols(self):
        return [v for b in self.batches for v in b[2]]


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def pipeline():
    return BatchPipeline()


@pytest.fixture
def trades_pd():
    if not HAS_PANDAS:
        pytest.skip("pandas not installed")
    return pd.DataFrame({
        "sym":    [1, 1, 2, 2, 3],
        "price":  [150.0, 151.0, 200.0, 201.0, 300.0],
        "volume": [100, 200, 150, 50, 300],
    })


@pytest.fixture
def trades_pl():
    if not HAS_POLARS:
        pytest.skip("polars not installed")
    return pl.DataFrame({
        "sym":    [1, 1, 2, 2, 3],
        "price":  [150.0, 151.0, 200.0, 201.0, 300.0],
        "volume": [100, 200, 150, 50, 300],
    })


@pytest.fixture
def trades_arrow():
    if not HAS_PYARROW:
        pytest.skip("pyarrow not installed")
    return pa.table({
        "sym":    pa.array([1, 1, 2, 2, 3], type=pa.int64()),
        "price":  pa.array([150.0, 151.0, 200.0, 201.0, 300.0], type=pa.float64()),
        "volume": pa.array([100, 200, 150, 50, 300], type=pa.int64()),
    })


# ============================================================================
# from_pandas tests
# ============================================================================

@pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
class TestFromPandas:

    def test_returns_row_count(self, pipeline, trades_pd):
        n = from_pandas(trades_pd, pipeline)
        assert n == 5

    def test_ingest_batch_called(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        assert len(pipeline.batches) >= 1

    def test_drain_called(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        assert pipeline.drain_calls >= 1

    def test_all_rows_ingested(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        assert pipeline.total_rows == 5

    def test_sym_values_correct(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        assert pipeline.all_syms == [1, 1, 2, 2, 3]

    def test_price_truncated_to_int(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        # Default price_scale=1.0, float 150.0 → int64 150
        assert pipeline.all_prices[0] == 150
        # Value must be integral (np.int64 or int are both acceptable)
        assert int(pipeline.all_prices[0]) == pipeline.all_prices[0]

    def test_price_scale(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline, price_scale=100)
        # 150.0 * 100 = 15000
        assert pipeline.all_prices[0] == 15000

    def test_vol_values_correct(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline)
        assert pipeline.all_vols == [100, 200, 150, 50, 300]

    def test_batching(self, pipeline, trades_pd):
        from_pandas(trades_pd, pipeline, batch_size=2)
        # 5 rows / 2 per batch → 3 batches
        assert len(pipeline.batches) == 3
        assert pipeline.total_rows == 5

    def test_empty_dataframe(self, pipeline):
        df = pd.DataFrame({"sym": [], "price": [], "volume": []})
        n = from_pandas(df, pipeline)
        assert n == 0
        assert pipeline.total_rows == 0

    def test_missing_column_raises(self, pipeline):
        df = pd.DataFrame({"sym": [1], "price": [100.0]})  # no "volume"
        with pytest.raises(ValueError, match="Missing columns"):
            from_pandas(df, pipeline)

    def test_custom_column_names(self, pipeline):
        df = pd.DataFrame({
            "symbol": [1, 2],
            "px":     [100.0, 200.0],
            "qty":    [10, 20],
        })
        n = from_pandas(df, pipeline, sym_col="symbol", price_col="px", vol_col="qty")
        assert n == 2
        assert pipeline.all_syms == [1, 2]

    def test_large_dataframe(self, pipeline):
        n = 100_000
        df = pd.DataFrame({
            "sym":    np.random.randint(1, 100, n),
            "price":  np.random.uniform(100, 200, n),
            "volume": np.random.randint(1, 1000, n),
        })
        ingested = from_pandas(df, pipeline)
        assert ingested == n
        assert pipeline.total_rows == n


# ============================================================================
# from_polars tests
# ============================================================================

@pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
class TestFromPolars:

    def test_returns_row_count(self, pipeline, trades_pl):
        n = from_polars(trades_pl, pipeline)
        assert n == 5

    def test_ingest_batch_called(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline)
        assert len(pipeline.batches) >= 1

    def test_all_rows_ingested(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline)
        assert pipeline.total_rows == 5

    def test_sym_values_correct(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline)
        assert pipeline.all_syms == [1, 1, 2, 2, 3]

    def test_price_truncated_to_int(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline)
        assert pipeline.all_prices[0] == 150

    def test_price_scale(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline, price_scale=100)
        assert pipeline.all_prices[0] == 15000

    def test_batching(self, pipeline, trades_pl):
        from_polars(trades_pl, pipeline, batch_size=2)
        assert len(pipeline.batches) == 3
        assert pipeline.total_rows == 5

    def test_empty_dataframe(self, pipeline):
        df = pl.DataFrame({"sym": [], "price": [], "volume": []})
        n = from_polars(df, pipeline)
        assert n == 0

    def test_missing_column_raises(self, pipeline):
        df = pl.DataFrame({"sym": [1], "price": [100.0]})  # no "volume"
        with pytest.raises(ValueError, match="Missing columns"):
            from_polars(df, pipeline)

    def test_custom_column_names(self, pipeline):
        df = pl.DataFrame({
            "symbol": [1, 2],
            "px":     [100.0, 200.0],
            "qty":    [10, 20],
        })
        n = from_polars(df, pipeline, sym_col="symbol", price_col="px", vol_col="qty")
        assert n == 2

    def test_large_dataframe(self, pipeline):
        n = 100_000
        df = pl.DataFrame({
            "sym":    [i % 100 for i in range(n)],
            "price":  [100.0 + i % 100 for i in range(n)],
            "volume": [i % 1000 + 1 for i in range(n)],
        })
        ingested = from_polars(df, pipeline)
        assert ingested == n
        assert pipeline.total_rows == n


# ============================================================================
# from_polars_arrow tests
# ============================================================================

@pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
class TestFromPolarsArrow:

    def test_returns_row_count(self, pipeline, trades_pl):
        n = from_polars_arrow(trades_pl, pipeline)
        assert n == 5

    def test_all_rows_ingested(self, pipeline, trades_pl):
        from_polars_arrow(trades_pl, pipeline)
        assert pipeline.total_rows == 5

    def test_sym_values_correct(self, pipeline, trades_pl):
        from_polars_arrow(trades_pl, pipeline)
        assert pipeline.all_syms == [1, 1, 2, 2, 3]

    def test_price_scale(self, pipeline, trades_pl):
        from_polars_arrow(trades_pl, pipeline, price_scale=100)
        assert pipeline.all_prices[0] == 15000

    def test_results_match_from_polars(self, trades_pl):
        p1 = BatchPipeline()
        p2 = BatchPipeline()
        from_polars(trades_pl, p1)
        from_polars_arrow(trades_pl, p2)
        assert p1.all_syms   == p2.all_syms
        assert p1.all_prices == p2.all_prices
        assert p1.all_vols   == p2.all_vols


# ============================================================================
# from_arrow tests
# ============================================================================

@pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow not installed")
class TestFromArrow:

    def test_returns_row_count(self, pipeline, trades_arrow):
        n = from_arrow(trades_arrow, pipeline)
        assert n == 5

    def test_ingest_batch_called(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline)
        assert len(pipeline.batches) >= 1

    def test_all_rows_ingested(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline)
        assert pipeline.total_rows == 5

    def test_sym_values_correct(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline)
        assert pipeline.all_syms == [1, 1, 2, 2, 3]

    def test_price_truncated_to_int(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline)
        assert pipeline.all_prices[0] == 150

    def test_price_scale(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline, price_scale=100)
        assert pipeline.all_prices[0] == 15000

    def test_vol_values_correct(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline)
        assert pipeline.all_vols == [100, 200, 150, 50, 300]

    def test_batching(self, pipeline, trades_arrow):
        from_arrow(trades_arrow, pipeline, batch_size=2)
        assert len(pipeline.batches) == 3
        assert pipeline.total_rows == 5

    def test_empty_table(self, pipeline):
        tbl = pa.table({
            "sym":    pa.array([], type=pa.int64()),
            "price":  pa.array([], type=pa.float64()),
            "volume": pa.array([], type=pa.int64()),
        })
        n = from_arrow(tbl, pipeline)
        assert n == 0

    def test_missing_column_raises(self, pipeline):
        tbl = pa.table({"sym": pa.array([1]), "price": pa.array([100.0])})
        with pytest.raises(ValueError, match="Missing columns"):
            from_arrow(tbl, pipeline)

    def test_custom_column_names(self, pipeline):
        tbl = pa.table({
            "symbol": pa.array([1, 2], type=pa.int64()),
            "px":     pa.array([100.0, 200.0], type=pa.float64()),
            "qty":    pa.array([10, 20], type=pa.int64()),
        })
        n = from_arrow(tbl, pipeline, sym_col="symbol", price_col="px", vol_col="qty")
        assert n == 2
        assert pipeline.all_syms == [1, 2]

    def test_integer_prices_no_scale(self, pipeline):
        tbl = pa.table({
            "sym":    pa.array([1], type=pa.int64()),
            "price":  pa.array([15000], type=pa.int64()),
            "volume": pa.array([100], type=pa.int64()),
        })
        from_arrow(tbl, pipeline)
        assert pipeline.all_prices[0] == 15000

    @pytest.mark.skipif(not HAS_NUMPY, reason="numpy required")
    def test_large_table(self, pipeline):
        n = 100_000
        tbl = pa.table({
            "sym":    pa.array(np.random.randint(1, 100, n), type=pa.int64()),
            "price":  pa.array(np.random.uniform(100, 200, n), type=pa.float64()),
            "volume": pa.array(np.random.randint(1, 1000, n), type=pa.int64()),
        })
        ingested = from_arrow(tbl, pipeline)
        assert ingested == n


# ============================================================================
# _require_cols helper
# ============================================================================

@pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
class TestRequireCols:

    def test_ok_when_all_present(self):
        df = pd.DataFrame({"a": [1], "b": [2], "c": [3]})
        _require_cols(df, ["a", "b"], "test")  # no raise

    def test_raises_on_missing(self):
        df = pd.DataFrame({"a": [1]})
        with pytest.raises(ValueError, match="Missing columns"):
            _require_cols(df, ["a", "b"], "test")

    def test_error_message_lists_missing(self):
        df = pd.DataFrame({"a": [1]})
        with pytest.raises(ValueError, match="b"):
            _require_cols(df, ["a", "b"], "test")

    @pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
    def test_polars_dataframe(self):
        df = pl.DataFrame({"x": [1], "y": [2]})
        _require_cols(df, ["x"], "polars")  # no raise
        with pytest.raises(ValueError):
            _require_cols(df, ["x", "z"], "polars")

    @pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow not installed")
    def test_arrow_table(self):
        tbl = pa.table({"a": [1], "b": [2.0]})
        _require_cols(tbl, ["a", "b"], "arrow")  # no raise
        with pytest.raises(ValueError):
            _require_cols(tbl, ["a", "c"], "arrow")

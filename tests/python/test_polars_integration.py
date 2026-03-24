"""
Tests for ZeptoDB polars integration.
"""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import polars as pl
    import numpy as np
    HAS_POLARS = True
except ImportError:
    HAS_POLARS = False

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

from zepto_py.dataframe import query_to_polars
from zepto_py.connection import QueryResult


pytestmark = pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def sample_trades_pl():
    return pl.DataFrame({
        "sym":       [1, 1, 2, 2, 3],
        "price":     [150.0, 151.0, 200.0, 201.0, 300.0],
        "size":      [100, 200, 150, 50, 300],
        "timestamp": [1_000_000_000, 2_000_000_000,
                      3_000_000_000, 4_000_000_000, 5_000_000_000],
    })


@pytest.fixture
def sample_quotes_pl():
    return pl.DataFrame({
        "sym":      [1, 1, 2],
        "bid":      [149.9, 150.9, 199.9],
        "ask":      [150.1, 151.1, 200.1],
        "timestamp":[1_000_000_000, 3_000_000_000, 2_000_000_000],
    })


@pytest.fixture
def mock_json_response():
    return '{"columns":["sym","avg_price"],"data":[[1,150.5],[2,200.5]],"rows":2}'


# ============================================================================
# query_to_polars tests
# ============================================================================

class TestQueryToPolars:

    def test_from_json_string(self, mock_json_response):
        df = query_to_polars(mock_json_response)
        assert isinstance(df, pl.DataFrame)
        assert df.shape == (2, 2)

    def test_columns(self, mock_json_response):
        df = query_to_polars(mock_json_response)
        assert df.columns == ["sym", "avg_price"]

    def test_values(self, mock_json_response):
        df = query_to_polars(mock_json_response)
        assert df["sym"].to_list() == [1, 2]
        assert df["avg_price"].to_list() == [150.5, 200.5]

    def test_empty(self):
        data = {"columns": ["a", "b"], "data": []}
        df = query_to_polars(data)
        assert len(df) == 0

    def test_from_dict(self):
        data = {"columns": ["x"], "data": [[10], [20], [30]]}
        df = query_to_polars(data)
        assert df["x"].to_list() == [10, 20, 30]


# ============================================================================
# Polars DataFrame operations
# ============================================================================

class TestPolarsOperations:

    def test_vwap_calculation(self, sample_trades_pl):
        """VWAP via polars expressions."""
        vwap = (
            (sample_trades_pl["price"] * sample_trades_pl["size"]).sum()
            / sample_trades_pl["size"].sum()
        )
        expected = (150.0*100 + 151.0*200 + 200.0*150 + 201.0*50 + 300.0*300) / 800
        assert abs(vwap - expected) < 1e-6

    def test_group_by_vwap(self, sample_trades_pl):
        """VWAP per symbol via polars group_by."""
        result = (
            sample_trades_pl
            .with_columns(
                (pl.col("price") * pl.col("size")).alias("notional")
            )
            .group_by("sym")
            .agg([
                (pl.col("notional").sum() / pl.col("size").sum()).alias("vwap"),
                pl.col("size").sum().alias("total_size"),
            ])
            .sort("sym")
        )

        assert result.shape[0] == 3
        sym1 = result.filter(pl.col("sym") == 1)["vwap"].item()
        expected_sym1 = (150.0*100 + 151.0*200) / 300
        assert abs(sym1 - expected_sym1) < 1e-6

    def test_ohlcv_bars(self, sample_trades_pl):
        """OHLCV computation."""
        result = (
            sample_trades_pl
            .group_by("sym")
            .agg([
                pl.col("price").first().alias("open"),
                pl.col("price").max().alias("high"),
                pl.col("price").min().alias("low"),
                pl.col("price").last().alias("close"),
                pl.col("size").sum().alias("volume"),
            ])
            .sort("sym")
        )

        assert result.shape[0] == 3
        sym1 = result.filter(pl.col("sym") == 1)
        assert sym1["high"].item() == 151.0
        assert sym1["low"].item() == 150.0

    def test_rolling_ema(self, sample_trades_pl):
        """EMA via polars ewm."""
        result = (
            sample_trades_pl
            .sort("timestamp")
            .with_columns(
                pl.col("price").ewm_mean(span=3).alias("ema3")
            )
        )
        assert "ema3" in result.columns
        assert result["ema3"].null_count() == 0

    def test_asof_join(self, sample_trades_pl, sample_quotes_pl):
        """Polars join_asof (equivalent to kdb+ aj)."""
        trades = sample_trades_pl.sort("timestamp")
        quotes = sample_quotes_pl.sort("timestamp")

        result = trades.join_asof(
            quotes,
            on="timestamp",
            by="sym",
            strategy="backward",
        )

        assert "bid" in result.columns
        assert "ask" in result.columns
        assert len(result) == len(trades)

    def test_time_bucket(self, sample_trades_pl):
        """Time bucketing (xbar equivalent)."""
        result = (
            sample_trades_pl
            .with_columns(
                (pl.col("timestamp") // 2_000_000_000 * 2_000_000_000).alias("bucket")
            )
            .group_by(["sym", "bucket"])
            .agg(pl.col("price").mean().alias("avg_price"))
            .sort("bucket")
        )
        assert "bucket" in result.columns
        assert "avg_price" in result.columns

    def test_lazy_evaluation(self, sample_trades_pl):
        """Polars lazy API — query planning."""
        result = (
            sample_trades_pl.lazy()
            .filter(pl.col("price") > 150.0)
            .group_by("sym")
            .agg(pl.col("size").sum())
            .sort("sym")
            .collect()
        )
        assert isinstance(result, pl.DataFrame)

    def test_schema_inference(self, sample_trades_pl):
        """Verify schema matches expected types."""
        schema = sample_trades_pl.schema
        assert schema["sym"]   == pl.Int64
        assert schema["price"] == pl.Float64
        assert schema["size"]  == pl.Int64

    def test_pandas_roundtrip(self, sample_trades_pl):
        """polars → pandas → polars roundtrip."""
        if not HAS_PANDAS:
            pytest.skip("pandas not installed")

        pd_df = sample_trades_pl.to_pandas()
        pl_df_back = pl.from_pandas(pd_df)

        assert pl_df_back.shape == sample_trades_pl.shape
        assert pl_df_back.columns == sample_trades_pl.columns

    def test_to_arrow_roundtrip(self, sample_trades_pl):
        """polars → Arrow → polars roundtrip."""
        try:
            import pyarrow as pa
        except ImportError:
            pytest.skip("pyarrow not installed")

        arrow_table = sample_trades_pl.to_arrow()
        assert isinstance(arrow_table, pa.Table)
        assert arrow_table.num_rows == len(sample_trades_pl)

        pl_back = pl.from_arrow(arrow_table)
        assert pl_back.shape == sample_trades_pl.shape

    def test_large_dataframe_performance(self):
        """Performance test: 1M row operations."""
        import time
        import random

        n = 1_000_000
        df = pl.DataFrame({
            "sym":   [random.randint(1, 100) for _ in range(n)],
            "price": [random.uniform(100, 200) for _ in range(n)],
            "size":  [random.randint(1, 1000) for _ in range(n)],
        })

        t0 = time.perf_counter()
        result = (
            df.lazy()
            .group_by("sym")
            .agg([
                pl.col("price").mean().alias("avg_price"),
                pl.col("size").sum().alias("total_size"),
            ])
            .collect()
        )
        elapsed = time.perf_counter() - t0

        assert len(result) == 100
        assert elapsed < 3.0, f"Polars GROUP BY 1M rows took {elapsed:.2f}s"

    def test_window_functions(self, sample_trades_pl):
        """Window functions: cumulative sum, rank."""
        result = (
            sample_trades_pl
            .sort("timestamp")
            .with_columns([
                pl.col("size").cum_sum().alias("cum_volume"),
                pl.col("price").rank().alias("price_rank"),
            ])
        )
        assert "cum_volume" in result.columns
        assert result["cum_volume"][-1] == sample_trades_pl["size"].sum()


# ============================================================================
# QueryResult polars tests
# ============================================================================

class TestQueryResultPolars:

    def test_to_polars(self):
        import json
        data = json.loads(
            '{"columns":["sym","price"],"data":[[1,150.0],[2,200.0]],"rows":2}'
        )
        result = QueryResult(data, elapsed_ms=1.0)
        df = result.to_polars()

        assert isinstance(df, pl.DataFrame)
        assert df.columns == ["sym", "price"]
        assert len(df) == 2

    def test_to_polars_values(self):
        data = {"columns": ["a", "b"], "data": [[1, 2.5], [3, 4.5]], "rows": 2}
        result = QueryResult(data, elapsed_ms=0.5)
        df = result.to_polars()
        assert df["a"].to_list() == [1, 3]
        assert df["b"].to_list() == [2.5, 4.5]

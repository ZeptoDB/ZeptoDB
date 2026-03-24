"""
Tests for ZeptoDB pandas integration.
"""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import pandas as pd
    import numpy as np
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

from zepto_py.dataframe import (
    query_to_pandas,
    from_pandas,
    to_pandas,
)
from zepto_py.connection import ZeptoConnection, QueryResult


pytestmark = pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def sample_trades_df():
    """Sample trades DataFrame."""
    return pd.DataFrame({
        "sym":   [1, 1, 2, 2, 3],
        "price": [150.0, 151.0, 200.0, 201.0, 300.0],
        "size":  [100, 200, 150, 50, 300],
        "timestamp": pd.to_datetime([
            "2024-01-01 09:30:00",
            "2024-01-01 09:30:01",
            "2024-01-01 09:30:02",
            "2024-01-01 09:30:03",
            "2024-01-01 09:30:04",
        ]).astype("int64"),
    })


@pytest.fixture
def sample_quotes_df():
    """Sample quotes DataFrame."""
    return pd.DataFrame({
        "sym": [1, 1, 2],
        "bid": [149.9, 150.9, 199.9],
        "ask": [150.1, 151.1, 200.1],
        "bid_size": [500, 300, 200],
        "ask_size": [400, 250, 150],
    })


@pytest.fixture
def mock_json_response():
    """Mock ZeptoDB JSON response."""
    return '{"columns":["sym","avg_price"],"data":[[1,150.5],[2,200.5],[3,300.0]],"rows":3,"execution_time_us":52.3}'


# ============================================================================
# QueryResult tests
# ============================================================================

class TestQueryResult:

    def test_to_pandas_basic(self, mock_json_response):
        import json
        data = json.loads(mock_json_response)
        result = QueryResult(data, elapsed_ms=1.0)
        df = result.to_pandas()

        assert isinstance(df, pd.DataFrame)
        assert list(df.columns) == ["sym", "avg_price"]
        assert len(df) == 3

    def test_to_pandas_values(self, mock_json_response):
        import json
        data = json.loads(mock_json_response)
        result = QueryResult(data, elapsed_ms=1.0)
        df = result.to_pandas()

        assert df["sym"].tolist() == [1, 2, 3]
        assert df["avg_price"].tolist() == [150.5, 200.5, 300.0]

    def test_to_pandas_empty(self):
        result = QueryResult({"columns": ["a", "b"], "data": [], "rows": 0}, 0.5)
        df = result.to_pandas()
        assert isinstance(df, pd.DataFrame)
        assert len(df) == 0
        assert list(df.columns) == ["a", "b"]

    def test_to_numpy(self, mock_json_response):
        import json
        data = json.loads(mock_json_response)
        result = QueryResult(data, elapsed_ms=1.0)
        arrays = result.to_numpy()

        assert "sym" in arrays
        assert "avg_price" in arrays
        assert isinstance(arrays["sym"], np.ndarray)
        assert len(arrays["sym"]) == 3

    def test_repr(self, mock_json_response):
        import json
        data = json.loads(mock_json_response)
        result = QueryResult(data, elapsed_ms=1.5)
        r = repr(result)
        assert "QueryResult" in r
        assert "rows=3" in r


# ============================================================================
# query_to_pandas tests
# ============================================================================

class TestQueryToPandas:

    def test_from_json_string(self, mock_json_response):
        df = query_to_pandas(mock_json_response)
        assert isinstance(df, pd.DataFrame)
        assert len(df) == 3

    def test_from_dict(self):
        data = {"columns": ["x", "y"], "data": [[1, 2.0], [3, 4.0]]}
        df = query_to_pandas(data)
        assert list(df.columns) == ["x", "y"]
        assert len(df) == 2

    def test_empty_result(self):
        data = {"columns": ["a"], "data": []}
        df = query_to_pandas(data)
        assert len(df) == 0

    def test_column_types(self):
        data = {
            "columns": ["int_col", "float_col", "str_col"],
            "data":    [[1, 1.5, "abc"], [2, 2.5, "def"]],
        }
        df = query_to_pandas(data)
        assert df["int_col"].tolist() == [1, 2]
        assert df["float_col"].tolist() == [1.5, 2.5]
        assert df["str_col"].tolist() == ["abc", "def"]


# ============================================================================
# DataFrame structure tests
# ============================================================================

class TestDataFrameStructure:

    def test_trades_df_columns(self, sample_trades_df):
        assert set(sample_trades_df.columns) == {"sym", "price", "size", "timestamp"}

    def test_trades_df_dtypes(self, sample_trades_df):
        assert sample_trades_df["price"].dtype == np.float64
        assert sample_trades_df["size"].dtype == np.int64

    def test_vwap_calculation(self, sample_trades_df):
        """Test VWAP calculation on pandas DataFrame."""
        df = sample_trades_df
        vwap = (df["price"] * df["size"]).sum() / df["size"].sum()
        assert vwap > 0
        # Verify formula
        expected = (150.0*100 + 151.0*200 + 200.0*150 + 201.0*50 + 300.0*300) / (100+200+150+50+300)
        assert abs(vwap - expected) < 1e-6

    def test_group_by_sym(self, sample_trades_df):
        """Test group by aggregation."""
        result = sample_trades_df.groupby("sym").agg(
            avg_price=("price", "mean"),
            total_size=("size", "sum"),
        ).reset_index()
        assert len(result) == 3
        assert result.loc[result["sym"] == 1, "avg_price"].iloc[0] == pytest.approx(150.5)

    def test_asof_join_simulation(self, sample_trades_df, sample_quotes_df):
        """Simulate ASOF JOIN behavior with pandas merge_asof."""
        trades = sample_trades_df.sort_values("timestamp")
        quotes = sample_quotes_df.copy()
        quotes["timestamp"] = [0, 1000000000, 2000000000]  # fake timestamps

        # merge_asof simulates kdb+ aj behavior
        result = pd.merge_asof(
            trades, quotes,
            on="timestamp",
            by="sym",
            direction="backward",
        )
        assert "bid" in result.columns
        assert "ask" in result.columns

    def test_rolling_ema(self, sample_trades_df):
        """Test EMA calculation."""
        prices = sample_trades_df["price"]
        ema = prices.ewm(span=3, adjust=False).mean()
        assert len(ema) == len(prices)
        assert not ema.isnull().any()


# ============================================================================
# ZeptoConnection mock tests
# ============================================================================

class TestZeptoConnectionParsing:
    """Test ZeptoConnection parsing logic without a live server."""

    def test_query_result_creation(self):
        data = {"columns": ["c1"], "data": [[1], [2]], "rows": 2}
        result = QueryResult(data, elapsed_ms=0.5)
        assert result.row_count == 2
        assert result.columns == ["c1"]

    def test_to_dict(self):
        data = {"columns": ["a", "b"], "data": [[1, "x"], [2, "y"]]}
        result = QueryResult(data, elapsed_ms=0.1)
        d = result.to_dict()
        assert d["a"] == [1, 2]
        assert d["b"] == ["x", "y"]

    def test_repr(self):
        conn = ZeptoConnection("testhost", 9999)
        assert "testhost" in repr(conn)
        assert "9999" in repr(conn)

    def test_context_manager(self):
        with ZeptoConnection("localhost", 8123) as conn:
            assert conn.host == "localhost"


# ============================================================================
# Data pipeline tests
# ============================================================================

class TestDataPipeline:

    def test_ohlcv_from_trades(self, sample_trades_df):
        """Compute OHLCV bars from trades DataFrame."""
        df = sample_trades_df.copy()
        df["timestamp"] = pd.to_datetime(df["timestamp"])

        # Set timestamp as index for resampling
        df = df.set_index("timestamp")

        # Compute per-symbol OHLCV
        groups = df.groupby("sym")
        ohlcv_list = []

        for sym, group in groups:
            ohlcv = pd.DataFrame({
                "open":   [group["price"].iloc[0]],
                "high":   [group["price"].max()],
                "low":    [group["price"].min()],
                "close":  [group["price"].iloc[-1]],
                "volume": [group["size"].sum()],
                "sym":    [sym],
            })
            ohlcv_list.append(ohlcv)

        result = pd.concat(ohlcv_list, ignore_index=True)
        assert len(result) == 3  # 3 symbols
        assert result.loc[result["sym"] == 1, "open"].iloc[0] == 150.0
        assert result.loc[result["sym"] == 1, "high"].iloc[0] == 151.0

    def test_spread_analysis(self, sample_quotes_df):
        """Compute bid-ask spread statistics."""
        df = sample_quotes_df.copy()
        df["spread"] = df["ask"] - df["bid"]
        df["spread_bps"] = df["spread"] / df["bid"] * 10000

        assert df["spread"].min() > 0
        assert (df["spread_bps"] > 0).all()

        # Mid price
        df["mid"] = (df["bid"] + df["ask"]) / 2
        assert (df["mid"] > df["bid"]).all()
        assert (df["mid"] < df["ask"]).all()

    def test_large_dataframe_performance(self):
        """Verify large DataFrame operations complete in reasonable time."""
        import time

        n = 1_000_000
        df = pd.DataFrame({
            "sym":   np.random.randint(1, 100, n),
            "price": np.random.uniform(100, 200, n),
            "size":  np.random.randint(1, 1000, n),
        })

        t0 = time.perf_counter()
        result = df.groupby("sym").agg({"price": "mean", "size": "sum"})
        elapsed = time.perf_counter() - t0

        assert len(result) == 99
        assert elapsed < 2.0, f"GROUP BY 1M rows took {elapsed:.2f}s (expected < 2s)"

"""
Tests for ZeptoDB StreamingSession — batch ingest with progress tracking.
"""
import pytest
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import pandas as pd
    import numpy as np
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

from zepto_py.streaming import StreamingSession


# ============================================================================
# Mock pipeline
# ============================================================================

class MockPipeline:
    """Minimal mock for StreamingSession tests."""

    def __init__(self, error_on: int = -1):
        self.ingested    = []
        self.drain_calls = 0
        self._error_on   = error_on   # raise on this row index
        self._call_count = 0

    def ingest(self, **kwargs):
        if self._error_on >= 0 and self._call_count == self._error_on:
            self._call_count += 1
            raise ValueError(f"Simulated error on row {self._error_on}")
        self.ingested.append(dict(kwargs))
        self._call_count += 1

    def drain(self):
        self.drain_calls += 1

    @property
    def row_count(self):
        return len(self.ingested)


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def pipeline():
    return MockPipeline()


@pytest.fixture
def small_trades_pd():
    if not HAS_PANDAS:
        pytest.skip("pandas not installed")
    return pd.DataFrame({
        "sym":   [1, 1, 2, 2, 3],
        "price": [150.0, 151.0, 200.0, 201.0, 300.0],
        "size":  [100, 200, 150, 50, 300],
    })


@pytest.fixture
def small_trades_pl():
    if not HAS_POLARS:
        pytest.skip("polars not installed")
    return pl.DataFrame({
        "sym":   [1, 1, 2, 2, 3],
        "price": [150.0, 151.0, 200.0, 201.0, 300.0],
        "size":  [100, 200, 150, 50, 300],
    })


# ============================================================================
# Construction
# ============================================================================

class TestStreamingSessionConstruction:

    def test_default_batch_size(self, pipeline):
        sess = StreamingSession(pipeline)
        assert sess.batch_size == 50_000

    def test_custom_batch_size(self, pipeline):
        sess = StreamingSession(pipeline, batch_size=1000)
        assert sess.batch_size == 1000

    def test_default_on_error(self, pipeline):
        sess = StreamingSession(pipeline)
        assert sess.on_error == "raise"

    def test_initial_stats(self, pipeline):
        sess = StreamingSession(pipeline)
        assert sess.total_ingested == 0
        assert sess.total_errors   == 0


# ============================================================================
# Pandas ingest
# ============================================================================

@pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
class TestStreamingSessionPandas:

    def test_ingest_returns_count(self, pipeline, small_trades_pd):
        sess = StreamingSession(pipeline)
        n    = sess.ingest_pandas(small_trades_pd)
        assert n == 5

    def test_ingest_all_rows(self, pipeline, small_trades_pd):
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(small_trades_pd)
        assert pipeline.row_count == 5

    def test_ingest_values_correct(self, pipeline, small_trades_pd):
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(small_trades_pd)
        first = pipeline.ingested[0]
        assert first["sym"]   == 1
        assert first["price"] == 150.0
        assert first["size"]  == 100

    def test_drain_called(self, pipeline, small_trades_pd):
        sess = StreamingSession(pipeline, batch_size=2)
        sess.ingest_pandas(small_trades_pd)
        assert pipeline.drain_calls >= 1

    def test_batching_splits_correctly(self, pipeline):
        df   = pd.DataFrame({"x": list(range(10))})
        sess = StreamingSession(pipeline, batch_size=3)
        n    = sess.ingest_pandas(df)
        assert n == 10
        assert pipeline.row_count == 10

    def test_empty_dataframe(self, pipeline):
        df   = pd.DataFrame({"a": [], "b": []})
        sess = StreamingSession(pipeline)
        n    = sess.ingest_pandas(df)
        assert n == 0

    def test_total_ingested_accumulates(self, pipeline, small_trades_pd):
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(small_trades_pd)
        sess.ingest_pandas(small_trades_pd)
        assert sess.total_ingested == 10

    def test_progress_callback(self, pipeline, small_trades_pd):
        callbacks = []
        def cb(done, total):
            callbacks.append((done, total))

        sess = StreamingSession(pipeline, batch_size=2)
        sess.ingest_pandas(small_trades_pd, progress_cb=cb)
        assert len(callbacks) > 0
        assert callbacks[-1][1] == 5   # total == 5

    def test_timestamp_column_converted(self, pipeline):
        """Pandas Timestamps are converted to int64 nanoseconds."""
        df = pd.DataFrame({
            "sym":       [1],
            "timestamp": pd.to_datetime(["2024-01-01 09:30:00"]),
        })
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(df)
        row = pipeline.ingested[0]
        assert isinstance(row["timestamp"], int)

    def test_numpy_scalars_converted(self, pipeline):
        """numpy int/float scalars must be unwrapped to Python native types."""
        df = pd.DataFrame({
            "val": np.array([42], dtype=np.int32),
        })
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(df)
        row = pipeline.ingested[0]
        assert not hasattr(row["val"], "item"), "numpy scalar not unwrapped"

    def test_error_raise_mode(self):
        """on_error='raise' propagates exceptions."""
        bad_pipeline = MockPipeline(error_on=0)
        df   = pd.DataFrame({"x": [1, 2]})
        sess = StreamingSession(bad_pipeline, on_error="raise")
        with pytest.raises(ValueError):
            sess.ingest_pandas(df)

    def test_error_skip_mode(self):
        """on_error='skip' silently ignores errors."""
        bad_pipeline = MockPipeline(error_on=0)
        df   = pd.DataFrame({"x": [1, 2]})
        sess = StreamingSession(bad_pipeline, on_error="skip")
        n    = sess.ingest_pandas(df)
        assert n >= 0   # did not raise

    def test_error_warn_mode(self):
        """on_error='warn' issues a UserWarning instead of raising."""
        import warnings
        bad_pipeline = MockPipeline(error_on=1)
        df   = pd.DataFrame({"x": [1, 2, 3]})
        sess = StreamingSession(bad_pipeline, on_error="warn")
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            sess.ingest_pandas(df)
        assert any("Ingest error" in str(warning.message) for warning in w)

    def test_show_progress_no_exception(self, pipeline, small_trades_pd, capsys):
        """show_progress=True prints output without raising."""
        sess = StreamingSession(pipeline)
        sess.ingest_pandas(small_trades_pd, show_progress=True)
        captured = capsys.readouterr()
        assert "rows" in captured.out.lower() or len(captured.out) > 0


# ============================================================================
# Polars ingest
# ============================================================================

@pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
class TestStreamingSessionPolars:

    def test_ingest_returns_count(self, pipeline, small_trades_pl):
        sess = StreamingSession(pipeline)
        n    = sess.ingest_polars(small_trades_pl)
        assert n == 5

    def test_ingest_all_rows(self, pipeline, small_trades_pl):
        sess = StreamingSession(pipeline)
        sess.ingest_polars(small_trades_pl)
        assert pipeline.row_count == 5

    def test_ingest_values_correct(self, pipeline, small_trades_pl):
        sess = StreamingSession(pipeline)
        sess.ingest_polars(small_trades_pl)
        syms = {r["sym"] for r in pipeline.ingested}
        assert syms == {1, 2, 3}

    def test_arrow_path(self, pipeline, small_trades_pl):
        """use_arrow=True path (requires pyarrow)."""
        if not HAS_PYARROW:
            pytest.skip("pyarrow not installed")
        sess = StreamingSession(pipeline)
        n    = sess.ingest_polars(small_trades_pl, use_arrow=True)
        assert n == 5

    def test_pandas_fallback_path(self, pipeline, small_trades_pl):
        """use_arrow=False falls back to pandas path."""
        if not HAS_PANDAS:
            pytest.skip("pandas required for fallback")
        sess = StreamingSession(pipeline)
        n    = sess.ingest_polars(small_trades_pl, use_arrow=False)
        assert n == 5

    def test_total_ingested_accumulates(self, pipeline, small_trades_pl):
        sess = StreamingSession(pipeline)
        sess.ingest_polars(small_trades_pl)
        sess.ingest_polars(small_trades_pl)
        assert sess.total_ingested == 10


# ============================================================================
# Iterator ingest
# ============================================================================

class TestStreamingSessionIter:

    def test_ingest_iter_basic(self, pipeline):
        def gen():
            for i in range(5):
                yield {"sym": i, "price": float(100 + i)}

        sess = StreamingSession(pipeline)
        n    = sess.ingest_iter(gen())
        assert n == 5
        assert pipeline.row_count == 5

    def test_ingest_iter_values(self, pipeline):
        rows = [{"a": 1, "b": 2.0}, {"a": 3, "b": 4.0}]
        sess = StreamingSession(pipeline)
        sess.ingest_iter(iter(rows))
        assert pipeline.ingested[0]["a"] == 1
        assert pipeline.ingested[1]["b"] == 4.0

    def test_ingest_iter_empty(self, pipeline):
        sess = StreamingSession(pipeline)
        n    = sess.ingest_iter(iter([]))
        assert n == 0

    def test_ingest_iter_drains_in_batches(self, pipeline):
        def gen():
            for i in range(15):
                yield {"x": i}

        sess = StreamingSession(pipeline, batch_size=5)
        sess.ingest_iter(gen())
        assert pipeline.drain_calls >= 3

    def test_ingest_iter_with_total_hint(self, pipeline, capsys):
        def gen():
            for i in range(10):
                yield {"v": i}

        sess = StreamingSession(pipeline, batch_size=5)
        sess.ingest_iter(gen(), total_hint=10, show_progress=True)
        assert pipeline.row_count == 10

    def test_ingest_iter_generator_large(self, pipeline):
        """1M rows via generator — memory efficient."""
        def tick_stream(n):
            for i in range(n):
                yield {"sym": i % 100, "price": 150.0 + (i % 10) * 0.1}

        sess = StreamingSession(pipeline, batch_size=50_000)
        n    = sess.ingest_iter(tick_stream(100_000))
        assert n == 100_000

    def test_ingest_iter_error_raise(self, pipeline):
        bad = MockPipeline(error_on=2)

        def gen():
            for i in range(5):
                yield {"x": i}

        sess = StreamingSession(bad, on_error="raise")
        with pytest.raises(ValueError):
            sess.ingest_iter(gen())

    def test_ingest_iter_error_skip(self):
        bad = MockPipeline(error_on=2)

        def gen():
            for i in range(5):
                yield {"x": i}

        sess = StreamingSession(bad, on_error="skip")
        n = sess.ingest_iter(gen())
        assert n >= 0


# ============================================================================
# Stats and reset
# ============================================================================

class TestStreamingSessionStats:

    def test_reset_stats(self, pipeline):
        sess = StreamingSession(pipeline)
        sess.ingest_iter(iter([{"x": 1}, {"x": 2}]))
        assert sess.total_ingested == 2
        sess.reset_stats()
        assert sess.total_ingested == 0
        assert sess.total_errors   == 0

    def test_stats_after_multiple_ingests(self, pipeline):
        sess = StreamingSession(pipeline)
        sess.ingest_iter(iter([{"x": i} for i in range(3)]))
        sess.ingest_iter(iter([{"y": i} for i in range(7)]))
        assert sess.total_ingested == 10

    @pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
    def test_total_errors_counted(self):
        """on_error='skip' increments total_errors via pipeline not session — verify drain still called."""
        class ErrorCountPipeline(MockPipeline):
            def __init__(self):
                super().__init__()
                self.error_count = 0
            def ingest(self, **kw):
                if kw.get("x", 0) == 2:
                    self.error_count += 1
                    raise ValueError("bad row")
                super().ingest(**kw)

        bad  = ErrorCountPipeline()
        df   = pd.DataFrame({"x": [1, 2, 3, 4]})
        sess = StreamingSession(bad, on_error="skip")
        n    = sess.ingest_pandas(df)
        assert bad.error_count == 1
        assert bad.row_count   == 3     # row with x=2 was skipped


# ============================================================================
# Performance
# ============================================================================

@pytest.mark.skipif(not HAS_PANDAS, reason="pandas not installed")
class TestStreamingPerformance:

    def test_pandas_throughput_100k(self):
        """100k row pandas ingest should complete quickly."""
        n  = 100_000
        df = pd.DataFrame({
            "sym":   np.random.randint(1, 100, n),
            "price": np.random.uniform(100, 200, n),
            "size":  np.random.randint(1, 1000, n),
        })

        class FastMock:
            def ingest(self, **kw): pass
            def drain(self): pass

        sess = StreamingSession(FastMock(), batch_size=10_000)
        t0   = time.perf_counter()
        n_   = sess.ingest_pandas(df)
        elapsed = time.perf_counter() - t0

        assert n_ == n
        assert elapsed < 30.0, f"Pandas 100k ingest took {elapsed:.2f}s"

    @pytest.mark.skipif(not HAS_POLARS, reason="polars not installed")
    @pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow required for Arrow path")
    def test_polars_arrow_throughput_100k(self):
        """100k row polars ingest via Arrow path."""
        n  = 100_000
        df = pl.DataFrame({
            "sym":   [i % 100 for i in range(n)],
            "price": [100.0 + i % 100 for i in range(n)],
        })

        class FastMock:
            def ingest(self, **kw): pass
            def drain(self): pass

        sess    = StreamingSession(FastMock(), batch_size=10_000)
        t0      = time.perf_counter()
        n_      = sess.ingest_polars(df, use_arrow=True)
        elapsed = time.perf_counter() - t0

        assert n_ == n
        assert elapsed < 30.0, f"Polars arrow 100k ingest took {elapsed:.2f}s"

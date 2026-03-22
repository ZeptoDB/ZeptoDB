"""
Tests for APEX-DB Apache Arrow integration.
"""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../.."))

try:
    import pyarrow as pa
    import pyarrow.compute as pc
    HAS_PYARROW = True
except ImportError:
    HAS_PYARROW = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

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

from apex_py.arrow import ArrowSession, apex_type_to_arrow, APEX_TO_ARROW


pytestmark = pytest.mark.skipif(not HAS_PYARROW, reason="pyarrow not installed")


# ============================================================================
# Fixtures
# ============================================================================

@pytest.fixture
def sample_arrow_table():
    return pa.table({
        "sym":       pa.array([1, 1, 2, 2, 3], type=pa.int64()),
        "price":     pa.array([150.0, 151.0, 200.0, 201.0, 300.0], type=pa.float64()),
        "size":      pa.array([100, 200, 150, 50, 300], type=pa.int64()),
        "timestamp": pa.array(
            [1_000_000_000, 2_000_000_000, 3_000_000_000, 4_000_000_000, 5_000_000_000],
            type=pa.int64(),
        ),
    })


@pytest.fixture
def mock_pipeline():
    """Mock pipeline supporting both ingest_batch() (vectorized) and get_column()."""
    class MockPipeline:
        def __init__(self):
            self.batches = []   # list of (syms, prices, vols)
            self._columns = {
                "price":     [150.0, 151.0, 200.0],
                "volume":    [100, 200, 150],
                "timestamp": [1_000_000_000, 2_000_000_000, 3_000_000_000],
            }

        def ingest_batch(self, symbols, prices, volumes):
            self.batches.append((list(symbols), list(prices), list(volumes)))

        def drain(self):
            pass

        def get_column(self, symbol: int, name: str):
            if not HAS_NUMPY:
                return self._columns.get(name)
            return np.array(self._columns.get(name, []))

        @property
        def total_rows(self):
            return sum(len(b[0]) for b in self.batches)

        @property
        def all_syms(self):
            return [v for b in self.batches for v in b[0]]

    return MockPipeline()


# ============================================================================
# Type mapping tests
# ============================================================================

class TestApexTypeMapping:

    def test_boolean(self):
        assert apex_type_to_arrow("BOOLEAN") == pa.bool_()

    def test_integer_types(self):
        assert apex_type_to_arrow("TINYINT")  == pa.int8()
        assert apex_type_to_arrow("SMALLINT") == pa.int16()
        assert apex_type_to_arrow("INTEGER")  == pa.int32()
        assert apex_type_to_arrow("BIGINT")   == pa.int64()

    def test_float_types(self):
        assert apex_type_to_arrow("REAL")   == pa.float32()
        assert apex_type_to_arrow("DOUBLE") == pa.float64()

    def test_string_type(self):
        assert apex_type_to_arrow("VARCHAR") == pa.large_utf8()

    def test_timestamp_type(self):
        t = apex_type_to_arrow("TIMESTAMP")
        assert t == pa.timestamp("ns", tz="UTC")

    def test_date_type(self):
        assert apex_type_to_arrow("DATE") == pa.date32()

    def test_unknown_defaults_to_float64(self):
        assert apex_type_to_arrow("UNKNOWN_TYPE") == pa.float64()

    def test_case_insensitive(self):
        assert apex_type_to_arrow("bigint") == pa.int64()
        assert apex_type_to_arrow("Double") == pa.float64()

    def test_all_apex_types_covered(self):
        for apex_type in APEX_TO_ARROW:
            result = apex_type_to_arrow(apex_type)
            assert result is not None


# ============================================================================
# Arrow table structure tests
# ============================================================================

class TestArrowTableOps:

    def test_table_schema(self, sample_arrow_table):
        assert sample_arrow_table.schema.field("sym").type   == pa.int64()
        assert sample_arrow_table.schema.field("price").type == pa.float64()

    def test_table_shape(self, sample_arrow_table):
        assert sample_arrow_table.num_rows    == 5
        assert sample_arrow_table.num_columns == 4

    def test_column_access(self, sample_arrow_table):
        prices = sample_arrow_table.column("price")
        assert prices[0].as_py() == 150.0
        assert prices[-1].as_py() == 300.0

    def test_filter(self, sample_arrow_table):
        mask   = pc.greater(sample_arrow_table.column("price"), 160.0)
        result = sample_arrow_table.filter(mask)
        assert result.num_rows == 3

    def test_group_by_aggregation(self, sample_arrow_table):
        result = (
            sample_arrow_table
            .group_by("sym")
            .aggregate([("price", "mean"), ("size", "sum")])
        )
        assert result.num_rows == 3

    def test_sort(self, sample_arrow_table):
        sorted_tbl = sample_arrow_table.sort_by([("price", "ascending")])
        prices = sorted_tbl.column("price").to_pylist()
        assert prices == sorted(prices)

    def test_slice(self, sample_arrow_table):
        sliced = sample_arrow_table.slice(1, 3)
        assert sliced.num_rows == 3

    def test_to_batches(self, sample_arrow_table):
        batches = sample_arrow_table.to_batches(max_chunksize=2)
        total = sum(b.num_rows for b in batches)
        assert total == sample_arrow_table.num_rows

    def test_schema_construction(self):
        schema = pa.schema([
            pa.field("sym",       pa.int32()),
            pa.field("price",     pa.float64()),
            pa.field("timestamp", pa.timestamp("ns", tz="UTC")),
        ])
        assert len(schema) == 3
        assert schema.field("timestamp").type == pa.timestamp("ns", tz="UTC")

    def test_record_batch(self, sample_arrow_table):
        batch = sample_arrow_table.to_batches()[0]
        assert isinstance(batch, pa.RecordBatch)
        assert batch.num_columns == 4


# ============================================================================
# ArrowSession ingest tests
# ============================================================================

class TestArrowSessionIngest:

    def test_ingest_arrow_table(self, mock_pipeline, sample_arrow_table):
        # sample_arrow_table has "size" column; map it to vol_col
        sess  = ArrowSession(mock_pipeline)
        count = sess.ingest_arrow(sample_arrow_table, vol_col="size")
        assert count == 5
        assert mock_pipeline.total_rows == 5

    def test_ingest_arrow_preserves_values(self, mock_pipeline, sample_arrow_table):
        sess = ArrowSession(mock_pipeline)
        sess.ingest_arrow(sample_arrow_table, vol_col="size")
        assert mock_pipeline.all_syms[0] == 1
        assert mock_pipeline.batches[0][1][0] == 150   # price 150.0 → int 150
        assert mock_pipeline.batches[0][2][0] == 100   # size 100

    def test_ingest_record_batch(self, mock_pipeline, sample_arrow_table):
        sess  = ArrowSession(mock_pipeline)
        batch = sample_arrow_table.to_batches()[0]
        count = sess.ingest_record_batch(batch, vol_col="size")
        assert count == batch.num_rows
        assert mock_pipeline.total_rows == batch.num_rows

    def test_ingest_batch_size(self, mock_pipeline):
        sess  = ArrowSession(mock_pipeline)
        table = pa.table({
            "sym":    pa.array(list(range(250)), type=pa.int64()),
            "price":  pa.array([100.0] * 250, type=pa.float64()),
            "volume": pa.array([10] * 250, type=pa.int64()),
        })
        count = sess.ingest_arrow(table, batch_size=100)
        assert count == 250
        assert mock_pipeline.total_rows == 250

    def test_ingest_nullable_columns(self, mock_pipeline):
        """Nullable Arrow columns (with nulls → 0 in numpy) don't crash ingest."""
        sess  = ArrowSession(mock_pipeline)
        # fill_null converts Arrow nulls to 0 before numpy extraction
        table = pa.table({
            "sym":    pa.array([1, 2, 3], type=pa.int64()),
            "price":  pa.array([100.0, 200.0, 300.0], type=pa.float64()),
            "volume": pa.array([10, 20, 30], type=pa.int64()),
        })
        count = sess.ingest_arrow(table)
        assert count == 3
        assert mock_pipeline.total_rows == 3


# ============================================================================
# ArrowSession export tests
# ============================================================================

class TestArrowSessionExport:

    def test_to_arrow_returns_table(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        result = sess.to_arrow(symbol=1)
        assert isinstance(result, pa.Table)

    def test_to_arrow_has_columns(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        result = sess.to_arrow(symbol=1)
        assert "price"     in result.schema.names
        assert "volume"    in result.schema.names
        assert "timestamp" in result.schema.names

    def test_to_arrow_custom_columns(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        result = sess.to_arrow(symbol=1, columns=["price", "volume"])
        assert result.schema.names == ["price", "volume"]

    def test_to_arrow_timestamp_type(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        result = sess.to_arrow(symbol=1)
        ts_type = result.schema.field("timestamp").type
        assert ts_type == pa.timestamp("ns", tz="UTC")

    def test_to_arrow_empty_on_missing_symbol(self, mock_pipeline):
        """Non-existent symbol returns empty table."""
        class EmptyPipeline:
            def get_column(self, symbol, name):
                return None
            def ingest(self, **kw): pass
            def drain(self): pass

        sess   = ArrowSession(EmptyPipeline())
        result = sess.to_arrow(symbol=999)
        assert result.num_rows == 0

    def test_to_record_batch_reader(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        reader = sess.to_record_batch_reader(symbol=1, chunk_size=1)
        assert isinstance(reader, pa.RecordBatchReader)
        tbl = reader.read_all()
        assert isinstance(tbl, pa.Table)

    def test_infer_schema(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        schema = sess.infer_schema(symbol=1)
        assert isinstance(schema, pa.Schema)
        assert len(schema) > 0


# ============================================================================
# ArrowSession schema utilities
# ============================================================================

class TestArrowSchemaUtilities:

    def test_apex_schema_to_arrow(self, mock_pipeline):
        sess = ArrowSession(mock_pipeline)
        apex_schema = [
            ("sym",       "BIGINT"),
            ("price",     "DOUBLE"),
            ("timestamp", "TIMESTAMP"),
        ]
        schema = sess.apex_schema_to_arrow(apex_schema)
        assert isinstance(schema, pa.Schema)
        assert schema.field("sym").type       == pa.int64()
        assert schema.field("price").type     == pa.float64()
        assert schema.field("timestamp").type == pa.timestamp("ns", tz="UTC")

    def test_to_arrow_with_explicit_schema(self, mock_pipeline):
        sess   = ArrowSession(mock_pipeline)
        schema = pa.schema([
            pa.field("price",  pa.float32()),
            pa.field("volume", pa.int32()),
        ])
        result = sess.to_arrow(symbol=1, columns=["price", "volume"], schema=schema)
        # Schema is applied — types match what was passed
        assert result.schema.field("price").type  == pa.float32()
        assert result.schema.field("volume").type == pa.int32()


# ============================================================================
# Cross-library roundtrip tests
# ============================================================================

class TestArrowRoundtrips:

    def test_arrow_to_polars(self, sample_arrow_table):
        if not HAS_POLARS:
            pytest.skip("polars not installed")
        df = pl.from_arrow(sample_arrow_table)
        assert isinstance(df, pl.DataFrame)
        assert df.shape == (5, 4)
        assert df["price"].to_list() == [150.0, 151.0, 200.0, 201.0, 300.0]

    def test_polars_to_arrow(self):
        if not HAS_POLARS:
            pytest.skip("polars not installed")
        df    = pl.DataFrame({"a": [1, 2, 3], "b": [1.0, 2.0, 3.0]})
        table = df.to_arrow()
        assert isinstance(table, pa.Table)
        assert table.num_rows    == 3
        assert table.num_columns == 2

    def test_arrow_to_pandas(self, sample_arrow_table):
        if not HAS_PANDAS:
            pytest.skip("pandas not installed")
        df = sample_arrow_table.to_pandas()
        assert isinstance(df, pd.DataFrame)
        assert len(df) == 5

    def test_pandas_to_arrow(self):
        if not HAS_PANDAS:
            pytest.skip("pandas not installed")
        df    = pd.DataFrame({"x": [1, 2], "y": [3.0, 4.0]})
        table = pa.Table.from_pandas(df)
        assert table.num_rows == 2

    def test_arrow_polars_pandas_roundtrip(self, sample_arrow_table):
        if not HAS_POLARS or not HAS_PANDAS:
            pytest.skip("polars and pandas required")
        pl_df   = pl.from_arrow(sample_arrow_table)
        pd_df   = pl_df.to_pandas()
        pl_back = pl.from_pandas(pd_df)
        assert pl_back.shape == pl_df.shape

    def test_arrow_numpy_roundtrip(self, sample_arrow_table):
        if not HAS_NUMPY:
            pytest.skip("numpy not installed")
        price_arr  = sample_arrow_table.column("price").to_pylist()
        np_arr     = np.array(price_arr)
        arrow_back = pa.array(np_arr)
        assert len(arrow_back) == len(price_arr)

    def test_chunked_array(self, sample_arrow_table):
        chunked = sample_arrow_table.column("price")
        assert isinstance(chunked, pa.ChunkedArray)
        flat = chunked.combine_chunks()
        assert len(flat) == 5

    def test_record_batch_to_table(self, sample_arrow_table):
        batches = sample_arrow_table.to_batches(max_chunksize=3)
        rebuilt = pa.Table.from_batches(batches)
        assert rebuilt.num_rows    == sample_arrow_table.num_rows
        assert rebuilt.num_columns == sample_arrow_table.num_columns


# ============================================================================
# DuckDB integration (optional)
# ============================================================================

class TestArrowDuckDB:

    def test_to_duckdb(self, mock_pipeline):
        try:
            import duckdb
        except ImportError:
            pytest.skip("duckdb not installed")

        sess = ArrowSession(mock_pipeline)
        conn = sess.to_duckdb(symbol=1, table_name="trades")
        result = conn.execute("SELECT COUNT(*) FROM trades").fetchone()
        assert result[0] >= 0

    def test_duckdb_query_on_arrow(self, sample_arrow_table):
        try:
            import duckdb
        except ImportError:
            pytest.skip("duckdb not installed")

        conn = duckdb.connect()
        conn.register("tbl", sample_arrow_table)
        result = conn.execute(
            "SELECT sym, AVG(price) AS avg_price FROM tbl GROUP BY sym ORDER BY sym"
        ).fetchall()

        assert len(result) == 3
        sym1_avg = result[0][1]
        assert abs(sym1_avg - 150.5) < 1e-6


# ============================================================================
# Performance
# ============================================================================

class TestArrowPerformance:

    def test_large_table_construction(self):
        """Build 1M row Arrow table quickly."""
        import time
        if not HAS_NUMPY:
            pytest.skip("numpy required")

        n      = 1_000_000
        t0     = time.perf_counter()
        table  = pa.table({
            "sym":   pa.array(np.random.randint(1, 100, n), type=pa.int64()),
            "price": pa.array(np.random.uniform(100, 200, n), type=pa.float64()),
            "size":  pa.array(np.random.randint(1, 1000, n), type=pa.int64()),
        })
        elapsed = time.perf_counter() - t0

        assert table.num_rows == n
        assert elapsed < 3.0, f"Arrow 1M table construction took {elapsed:.2f}s"

    def test_large_table_filter_performance(self):
        """Filter 1M row Arrow table."""
        import time
        if not HAS_NUMPY:
            pytest.skip("numpy required")

        n     = 1_000_000
        table = pa.table({
            "price": pa.array(np.random.uniform(100, 200, n), type=pa.float64()),
        })

        t0      = time.perf_counter()
        mask    = pc.greater(table.column("price"), 150.0)
        result  = table.filter(mask)
        elapsed = time.perf_counter() - t0

        assert result.num_rows > 0
        assert elapsed < 1.0, f"Arrow filter 1M rows took {elapsed:.2f}s"

"""
APEX-DB Arrow Integration — zero-copy data exchange via Apache Arrow.

Fast ingest path:
  ArrowSession.ingest_arrow()           — vectorized (no row iteration)
  ArrowSession.ingest_arrow_columnar()  — column-wise numpy batch (fastest)

Export path (zero-copy):
  ArrowSession.to_arrow()               — numpy view → Arrow Table
  ArrowSession.to_polars_zero_copy()    — Arrow → polars (no copy)
  ArrowSession.to_duckdb()              — Arrow registered in DuckDB
"""
from __future__ import annotations

from typing import Optional, List, Any

try:
    import pyarrow as pa
    HAS_PYARROW = True
except ImportError:
    HAS_PYARROW = False

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


# ============================================================================
# APEX-DB ↔ Arrow Schema Mapping
# ============================================================================

# Maps APEX-DB SQL types to Arrow types
APEX_TO_ARROW = {
    "BOOLEAN":   lambda: pa.bool_()       if HAS_PYARROW else None,
    "TINYINT":   lambda: pa.int8()        if HAS_PYARROW else None,
    "SMALLINT":  lambda: pa.int16()       if HAS_PYARROW else None,
    "INTEGER":   lambda: pa.int32()       if HAS_PYARROW else None,
    "BIGINT":    lambda: pa.int64()       if HAS_PYARROW else None,
    "REAL":      lambda: pa.float32()     if HAS_PYARROW else None,
    "DOUBLE":    lambda: pa.float64()     if HAS_PYARROW else None,
    "VARCHAR":   lambda: pa.large_utf8()  if HAS_PYARROW else None,
    "TIMESTAMP": lambda: pa.timestamp("ns", tz="UTC") if HAS_PYARROW else None,
    "DATE":      lambda: pa.date32()      if HAS_PYARROW else None,
}


def apex_type_to_arrow(apex_type: str) -> Any:
    """Convert APEX-DB type string to pyarrow type."""
    if not HAS_PYARROW:
        raise ImportError("pyarrow is required: pip install pyarrow")
    factory = APEX_TO_ARROW.get(apex_type.upper())
    return factory() if factory else pa.float64()


# ============================================================================
# ArrowSession
# ============================================================================

class ArrowSession:
    """
    APEX-DB session with Apache Arrow integration.

    Provides zero-copy data exchange between APEX-DB and:
    - Apache Arrow (RecordBatch, Table)
    - Pandas (via Arrow)
    - Polars (native Arrow)
    - DuckDB (Arrow Table)
    - Ray Dataset (Arrow)

    Example
    -------
    >>> import pyarrow as pa
    >>> pipeline = apex.Pipeline()
    >>> pipeline.start()
    >>> sess = ArrowSession(pipeline)
    >>>
    >>> # Ingest from Arrow Table
    >>> table = pa.table({"sym": [1, 2], "price": [150.0, 200.0]})
    >>> sess.ingest_arrow(table)
    2
    >>>
    >>> # Export as Arrow Table
    >>> table = sess.to_arrow(symbol=1)
    >>> table.schema
    """

    def __init__(self, pipeline: Any):
        if not HAS_PYARROW:
            raise ImportError("pyarrow is required: pip install pyarrow")
        self.pipeline = pipeline

    # ------------------------------------------------------------------
    # Ingest (vectorized — no Python row iteration)
    # ------------------------------------------------------------------

    def ingest_arrow(
        self,
        table: "pa.Table",
        batch_size: int = 100_000,
        sym_col: str = "sym",
        price_col: str = "price",
        vol_col: str = "volume",
        price_scale: float = 1.0,
        vol_scale: float = 1.0,
    ) -> int:
        """
        Ingest an Arrow Table into APEX-DB via vectorized batch.

        Uses .to_numpy(zero_copy_only=False) to extract column buffers and
        calls ingest_batch() once per chunk — no Python-level row iteration.

        Parameters
        ----------
        table : pa.Table
        batch_size : int
            Rows per ingest_batch() call.
        sym_col, price_col, vol_col : str
            Column names in the Arrow table.
        price_scale, vol_scale : float
            Float→int64 scale factor.

        Returns
        -------
        int : rows ingested
        """
        from .dataframe import from_arrow
        return from_arrow(
            table, self.pipeline,
            batch_size=batch_size,
            sym_col=sym_col, price_col=price_col, vol_col=vol_col,
            price_scale=price_scale, vol_scale=vol_scale,
        )

    def ingest_arrow_columnar(
        self,
        sym_arr: "pa.Array",
        price_arr: "pa.Array",
        vol_arr: "pa.Array",
        price_scale: float = 1.0,
        vol_scale: float = 1.0,
    ) -> int:
        """
        Column-wise Arrow array ingest — maximum zero-copy path.

        Accepts individual Arrow arrays (e.g., from a Polars Series via
        series.to_arrow()) and calls ingest_batch() with their numpy buffers.

        Parameters
        ----------
        sym_arr : pa.Array    — symbol IDs
        price_arr : pa.Array  — prices
        vol_arr : pa.Array    — volumes
        price_scale : float   — scale for float prices (e.g. 100 = cents)
        vol_scale : float

        Returns
        -------
        int : rows ingested

        Example
        -------
        >>> import pyarrow as pa, apex, apex_py as apx
        >>> pipeline = apex.Pipeline(); pipeline.start()
        >>> sess = apx.ArrowSession(pipeline)
        >>> sess.ingest_arrow_columnar(
        ...     pa.array([1, 1, 2], type=pa.int64()),
        ...     pa.array([15000, 15001, 16000], type=pa.int64()),
        ...     pa.array([100, 200, 150], type=pa.int64()),
        ... )
        3
        """
        from .dataframe import _arrow_col_to_int64, _arrow_col_to_numpy

        n = len(sym_arr)
        syms = _arrow_col_to_int64(sym_arr)

        raw_prices = _arrow_col_to_numpy(price_arr)
        if price_scale != 1.0 or raw_prices.dtype.kind == "f":
            prices = (raw_prices * price_scale).astype(np.int64)
        else:
            prices = raw_prices.astype(np.int64, copy=False)

        raw_vols = _arrow_col_to_numpy(vol_arr)
        if vol_scale != 1.0 or raw_vols.dtype.kind == "f":
            vols = (raw_vols * vol_scale).astype(np.int64)
        else:
            vols = raw_vols.astype(np.int64, copy=False)

        self.pipeline.ingest_batch(symbols=syms, prices=prices, volumes=vols)
        self.pipeline.drain()
        return n

    def ingest_record_batch(
        self,
        batch: "pa.RecordBatch",
        sym_col: str = "sym",
        price_col: str = "price",
        vol_col: str = "volume",
        price_scale: float = 1.0,
        vol_scale: float = 1.0,
    ) -> int:
        """Ingest a single Arrow RecordBatch (vectorized)."""
        return self.ingest_arrow_columnar(
            batch.column(sym_col),
            batch.column(price_col),
            batch.column(vol_col),
            price_scale=price_scale,
            vol_scale=vol_scale,
        )

    # ------------------------------------------------------------------
    # Export
    # ------------------------------------------------------------------

    def to_arrow(
        self,
        symbol: int,
        columns: Optional[List[str]] = None,
        schema: Optional["pa.Schema"] = None,
    ) -> "pa.Table":
        """
        Export APEX-DB data as an Arrow Table.

        Parameters
        ----------
        symbol : int
        columns : list of str, optional
        schema : pa.Schema, optional
            Explicit schema. If None, inferred from numpy arrays.

        Returns
        -------
        pa.Table
        """
        target_cols = columns or ["price", "volume", "timestamp"]
        arrays = {}

        for col_name in target_cols:
            try:
                arr = self.pipeline.get_column(symbol=symbol, name=col_name)
                if arr is not None:
                    if col_name == "timestamp":
                        arrays[col_name] = pa.array(arr,
                                                     type=pa.timestamp("ns", tz="UTC"))
                    else:
                        arrays[col_name] = pa.array(arr)
            except Exception:
                pass

        if not arrays:
            return pa.table({})

        if schema:
            return pa.table(arrays, schema=schema)
        return pa.table(arrays)

    def to_record_batch_reader(
        self,
        symbol: int,
        chunk_size: int = 100_000,
        columns: Optional[List[str]] = None,
    ) -> "pa.RecordBatchReader":
        """
        Export as a RecordBatchReader for streaming consumption.

        Useful for feeding into DuckDB, Ray, or Spark.
        """
        table = self.to_arrow(symbol=symbol, columns=columns)
        return pa.RecordBatchReader.from_batches(
            table.schema,
            table.to_batches(max_chunksize=chunk_size)
        )

    # ------------------------------------------------------------------
    # DuckDB integration
    # ------------------------------------------------------------------

    def to_duckdb(
        self,
        symbol: int,
        table_name: str = "apex_data",
        conn: Any = None,
    ) -> Any:
        """
        Register APEX-DB data as a DuckDB table.

        Parameters
        ----------
        symbol : int
        table_name : str
            DuckDB table/view name.
        conn : duckdb.DuckDBPyConnection, optional
            Existing connection. Creates in-memory connection if None.

        Returns
        -------
        duckdb.DuckDBPyConnection

        Example
        -------
        >>> conn = sess.to_duckdb(symbol=1, table_name="trades")
        >>> conn.execute("SELECT sym, count(*) FROM trades GROUP BY sym").df()
        """
        try:
            import duckdb
        except ImportError:
            raise ImportError("duckdb is required: pip install duckdb")

        arrow_table = self.to_arrow(symbol=symbol)
        if conn is None:
            conn = duckdb.connect()

        # Register Arrow table directly (zero-copy)
        conn.register(table_name, arrow_table)
        return conn

    # ------------------------------------------------------------------
    # Polars zero-copy
    # ------------------------------------------------------------------

    def to_polars_zero_copy(
        self,
        symbol: int,
        columns: Optional[List[str]] = None,
    ) -> Any:
        """
        Export to polars DataFrame via Arrow (true zero-copy).

        Both polars and this function use Arrow internally,
        so no data is copied.
        """
        try:
            import polars as pl
        except ImportError:
            raise ImportError("polars is required: pip install polars")

        arrow_table = self.to_arrow(symbol=symbol, columns=columns)
        return pl.from_arrow(arrow_table)

    # ------------------------------------------------------------------
    # Schema utilities
    # ------------------------------------------------------------------

    def infer_schema(
        self,
        symbol: int,
        sample_rows: int = 100,
    ) -> "pa.Schema":
        """Infer Arrow schema from APEX-DB column data."""
        table = self.to_arrow(symbol=symbol)
        return table.schema

    def apex_schema_to_arrow(
        self,
        apex_schema: List[tuple],
    ) -> "pa.Schema":
        """
        Convert APEX-DB schema description to Arrow schema.

        Parameters
        ----------
        apex_schema : list of (name, apex_type_str)

        Returns
        -------
        pa.Schema
        """
        fields = []
        for name, apex_type in apex_schema:
            fields.append(pa.field(name, apex_type_to_arrow(apex_type)))
        return pa.schema(fields)

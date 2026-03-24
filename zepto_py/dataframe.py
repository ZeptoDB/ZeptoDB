"""
ZeptoDB DataFrame utilities — pandas/polars/Arrow conversion.

Fast paths (in order of performance):
  1. from_polars()       — polars .to_numpy() zero-copy + C++ ingest_batch()
  2. from_pandas()       — numpy vectorized extraction + C++ ingest_batch()
  3. from_arrow()        — Arrow → numpy + C++ ingest_batch()

All fast paths avoid Python-level row iteration (no iterrows / col[i].as_py()).

Column mapping defaults (sym_col, price_col, vol_col) can be overridden to
match any DataFrame schema.  Float prices are scaled to int64 via price_scale
(e.g. price_scale=100 stores cents; default 1 truncates to integer).
"""
from __future__ import annotations

from typing import Optional, Union, List, Any

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


# ============================================================================
# Pandas → ZeptoDB
# ============================================================================

def from_pandas(
    df: "pd.DataFrame",
    pipeline: Any,
    batch_size: int = 50_000,
    sym_col: str = "sym",
    price_col: str = "price",
    vol_col: str = "volume",
    price_scale: float = 1.0,
    vol_scale: float = 1.0,
    show_progress: bool = False,
) -> int:
    """
    Ingest a pandas DataFrame into ZeptoDB via the C++ pipeline.

    Uses vectorized numpy extraction + ingest_batch() instead of iterrows(),
    giving 100-1000x speedup for large DataFrames.

    Parameters
    ----------
    df : pd.DataFrame
        Source data. Must contain sym_col, price_col, vol_col columns.
    pipeline : ZeptoPipeline (C++ pybind11 object)
        ZeptoDB pipeline instance (zeptodb.Pipeline()).
    batch_size : int
        Rows per C++ batch call. Larger = fewer Python↔C++ crossings.
    sym_col : str
        Column name containing the symbol ID (int).
    price_col : str
        Column name containing the price.
    vol_col : str
        Column name containing the volume.
    price_scale : float
        Multiply float prices by this factor before converting to int64.
        E.g. price_scale=100 stores prices in cents (2 decimal places).
        Default 1.0 truncates float to int64.
    vol_scale : float
        Same scaling for volume. Default 1.0.
    show_progress : bool
        Print progress to stdout.

    Returns
    -------
    int : rows ingested

    Example
    -------
    >>> import zeptodb, zepto_py as apx, pandas as pd
    >>> pipeline = zeptodb.Pipeline(); pipeline.start()
    >>> df = pd.DataFrame({"sym": [1]*1000, "price": [150.25]*1000, "volume": [100]*1000})
    >>> apx.from_pandas(df, pipeline, price_scale=100)   # store cents
    1000
    """
    if not HAS_PANDAS:
        raise ImportError("pandas is required: pip install pandas")
    if not HAS_NUMPY:
        raise ImportError("numpy is required: pip install numpy")

    _require_cols(df, [sym_col, price_col, vol_col], "pandas DataFrame")

    total = len(df)
    ingested = 0

    for start in range(0, total, batch_size):
        chunk = df.iloc[start : start + batch_size]
        ingested += _ingest_pandas_chunk(
            chunk, pipeline, sym_col, price_col, vol_col, price_scale, vol_scale
        )
        if show_progress:
            done = min(start + batch_size, total)
            print(f"\rIngested {done}/{total}", end="", flush=True)

    if show_progress:
        print()

    return ingested


def _ingest_pandas_chunk(
    chunk: "pd.DataFrame",
    pipeline: Any,
    sym_col: str,
    price_col: str,
    vol_col: str,
    price_scale: float,
    vol_scale: float,
) -> int:
    """Vectorized ingest of one pandas chunk."""
    syms = chunk[sym_col].to_numpy(dtype=np.int64, copy=False)

    raw_prices = chunk[price_col].to_numpy(copy=False)
    if price_scale != 1.0 or raw_prices.dtype.kind == "f":
        prices = (raw_prices * price_scale).astype(np.int64)
    else:
        prices = raw_prices.astype(np.int64, copy=False)

    raw_vols = chunk[vol_col].to_numpy(copy=False)
    if vol_scale != 1.0 or raw_vols.dtype.kind == "f":
        vols = (raw_vols * vol_scale).astype(np.int64)
    else:
        vols = raw_vols.astype(np.int64, copy=False)

    pipeline.ingest_batch(symbols=syms, prices=prices, volumes=vols)
    pipeline.drain()
    return len(chunk)


def to_pandas(
    pipeline: Any,
    symbol: int,
    columns: Optional[List[str]] = None,
    start_ts: Optional[int] = None,
    end_ts: Optional[int] = None,
) -> "pd.DataFrame":
    """
    Export ZeptoDB data to a pandas DataFrame (zero-copy via numpy).

    Parameters
    ----------
    pipeline : ZeptoPipeline
    symbol : int
        Symbol ID to export.
    columns : list of str, optional
        Column names to export. None = ["price", "volume", "timestamp"].
    start_ts, end_ts : int, optional
        Nanosecond timestamps for range filter.

    Returns
    -------
    pd.DataFrame
    """
    if not HAS_PANDAS:
        raise ImportError("pandas is required: pip install pandas")
    if not HAS_NUMPY:
        raise ImportError("numpy is required: pip install numpy")

    target_cols = columns or ["price", "volume", "timestamp"]
    data: dict = {}

    for col_name in target_cols:
        try:
            arr = pipeline.get_column(symbol=symbol, name=col_name)
            if arr is not None:
                data[col_name] = arr  # zero-copy numpy view
        except Exception:
            pass

    df = pd.DataFrame(data)

    if "timestamp" in df.columns:
        df["timestamp"] = pd.to_datetime(df["timestamp"], unit="ns", utc=True)

    if start_ts is not None or end_ts is not None:
        if "timestamp" in df.columns:
            mask = pd.Series([True] * len(df), index=df.index)
            if start_ts is not None:
                mask &= df["timestamp"] >= pd.Timestamp(start_ts, unit="ns", tz="UTC")
            if end_ts is not None:
                mask &= df["timestamp"] <= pd.Timestamp(end_ts, unit="ns", tz="UTC")
            df = df[mask].reset_index(drop=True)

    return df


# ============================================================================
# Polars → ZeptoDB  (zero-copy fast path)
# ============================================================================

def from_polars(
    df: "pl.DataFrame",
    pipeline: Any,
    batch_size: int = 100_000,
    sym_col: str = "sym",
    price_col: str = "price",
    vol_col: str = "volume",
    price_scale: float = 1.0,
    vol_scale: float = 1.0,
    show_progress: bool = False,
) -> int:
    """
    Zero-copy Polars DataFrame → ZeptoDB ingest.

    Polars .to_numpy() on a numeric Series without nulls returns a numpy
    array backed directly by the Arrow buffer (no copy).  Combined with
    ingest_batch() this gives the minimum possible Python overhead.

    Parameters
    ----------
    df : pl.DataFrame
    pipeline : ZeptoPipeline
    batch_size : int, default 100_000
        Larger batches amortize C++ call overhead.
    sym_col : str
        Column with symbol IDs.
    price_col : str
        Column with prices.
    vol_col : str
        Column with volumes.
    price_scale : float
        Float→int64 scale factor (e.g. 100 for cents).
    vol_scale : float
        Same for volume.
    show_progress : bool

    Returns
    -------
    int : rows ingested

    Example
    -------
    >>> import polars as pl, zeptodb, zepto_py as apx
    >>> pipeline = zeptodb.Pipeline(); pipeline.start()
    >>> df = pl.DataFrame({"sym": [1]*1_000_000, "price": [15000]*1_000_000,
    ...                    "volume": [100]*1_000_000})
    >>> apx.from_polars(df, pipeline)   # ~zero-copy, single ingest_batch call
    1000000
    """
    if not HAS_POLARS:
        raise ImportError("polars is required: pip install polars")
    if not HAS_NUMPY:
        raise ImportError("numpy is required: pip install numpy")

    _require_cols(df, [sym_col, price_col, vol_col], "polars DataFrame")

    total = len(df)
    ingested = 0

    for start in range(0, total, batch_size):
        # df.slice() is zero-copy in Polars (returns a view)
        chunk = df.slice(start, batch_size)
        ingested += _ingest_polars_chunk(
            chunk, pipeline, sym_col, price_col, vol_col, price_scale, vol_scale
        )
        if show_progress:
            done = min(start + batch_size, total)
            print(f"\rIngested {done}/{total}", end="", flush=True)

    if show_progress:
        print()

    return ingested


def _ingest_polars_chunk(
    chunk: "pl.DataFrame",
    pipeline: Any,
    sym_col: str,
    price_col: str,
    vol_col: str,
    price_scale: float,
    vol_scale: float,
) -> int:
    """Vectorized ingest of one polars chunk.

    For numeric Series without nulls, .to_numpy() returns the Arrow buffer
    directly — no allocation, no copy.
    """
    # allow_copy=False would raise if copy is needed; we allow it for safety
    syms = chunk[sym_col].to_numpy().astype(np.int64, copy=False)

    raw_prices = chunk[price_col].to_numpy()
    if price_scale != 1.0 or raw_prices.dtype.kind == "f":
        prices = (raw_prices * price_scale).astype(np.int64)
    else:
        prices = raw_prices.astype(np.int64, copy=False)

    raw_vols = chunk[vol_col].to_numpy()
    if vol_scale != 1.0 or raw_vols.dtype.kind == "f":
        vols = (raw_vols * vol_scale).astype(np.int64)
    else:
        vols = raw_vols.astype(np.int64, copy=False)

    pipeline.ingest_batch(symbols=syms, prices=prices, volumes=vols)
    pipeline.drain()
    return len(chunk)


def from_polars_arrow(
    df: "pl.DataFrame",
    pipeline: Any,
    sym_col: str = "sym",
    price_col: str = "price",
    vol_col: str = "volume",
    price_scale: float = 1.0,
    vol_scale: float = 1.0,
) -> int:
    """
    Polars → Arrow → numpy zero-copy ingest (requires pyarrow).

    Uses df.to_arrow() (zero-copy in Polars) then Arrow .to_numpy()
    (zero-copy for contiguous primitive arrays without nulls).

    Falls back to from_polars() if pyarrow is unavailable.
    """
    if not HAS_POLARS:
        raise ImportError("polars is required")

    if not HAS_PYARROW:
        return from_polars(df, pipeline, sym_col=sym_col,
                           price_col=price_col, vol_col=vol_col,
                           price_scale=price_scale, vol_scale=vol_scale)

    _require_cols(df, [sym_col, price_col, vol_col], "polars DataFrame")

    # to_arrow() is zero-copy: Polars uses Arrow memory internally
    arrow_table = df.to_arrow()
    return from_arrow(arrow_table, pipeline,
                      sym_col=sym_col, price_col=price_col, vol_col=vol_col,
                      price_scale=price_scale, vol_scale=vol_scale)


def to_polars(
    pipeline: Any,
    symbol: int,
    columns: Optional[List[str]] = None,
) -> "pl.DataFrame":
    """
    Export ZeptoDB data to a polars DataFrame (zero-copy via numpy view).

    Parameters
    ----------
    pipeline : ZeptoPipeline
    symbol : int
    columns : list of str, optional

    Returns
    -------
    pl.DataFrame
    """
    if not HAS_POLARS:
        raise ImportError("polars is required: pip install polars")
    if not HAS_NUMPY:
        raise ImportError("numpy is required: pip install numpy")

    target_cols = columns or ["price", "volume", "timestamp"]
    series_list = []

    for col_name in target_cols:
        try:
            arr = pipeline.get_column(symbol=symbol, name=col_name)
            if arr is not None:
                if col_name == "timestamp":
                    s = pl.Series(col_name, arr, dtype=pl.Datetime("ns", "UTC"))
                else:
                    s = pl.Series(col_name, arr)
                series_list.append(s)
        except Exception:
            pass

    return pl.DataFrame(series_list) if series_list else pl.DataFrame()


# ============================================================================
# Arrow → ZeptoDB  (requires pyarrow)
# ============================================================================

def from_arrow(
    table: "pa.Table",
    pipeline: Any,
    batch_size: int = 100_000,
    sym_col: str = "sym",
    price_col: str = "price",
    vol_col: str = "volume",
    price_scale: float = 1.0,
    vol_scale: float = 1.0,
) -> int:
    """
    Apache Arrow Table → ZeptoDB vectorized ingest.

    Arrow .to_numpy(zero_copy_only=False) extracts column buffers with
    minimal copying (zero-copy when the array is contiguous without nulls).
    Combined with ingest_batch() this avoids all Python-level row iteration.

    Parameters
    ----------
    table : pa.Table
        Source Arrow table.
    pipeline : ZeptoPipeline
    batch_size : int, default 100_000
    sym_col, price_col, vol_col : str
        Column names.
    price_scale, vol_scale : float
        Float→int64 scaling.

    Returns
    -------
    int : rows ingested

    Example
    -------
    >>> import pyarrow as pa, zeptodb, zepto_py as apx
    >>> pipeline = zeptodb.Pipeline(); pipeline.start()
    >>> tbl = pa.table({"sym": [1, 2], "price": [15000, 16000], "volume": [100, 200]})
    >>> apx.from_arrow(tbl, pipeline)
    2
    """
    if not HAS_PYARROW:
        raise ImportError("pyarrow is required: pip install pyarrow")
    if not HAS_NUMPY:
        raise ImportError("numpy is required: pip install numpy")

    _require_cols(table, [sym_col, price_col, vol_col], "Arrow Table")

    total = table.num_rows
    ingested = 0

    for batch in table.to_batches(max_chunksize=batch_size):
        # Fill nulls before conversion so numpy cast doesn't warn
        syms = _arrow_col_to_int64(batch.column(sym_col))

        raw_prices = _arrow_col_to_numpy(batch.column(price_col))
        if price_scale != 1.0 or raw_prices.dtype.kind == "f":
            prices = (raw_prices * price_scale).astype(np.int64)
        else:
            prices = raw_prices.astype(np.int64, copy=False)

        raw_vols = _arrow_col_to_numpy(batch.column(vol_col))
        if vol_scale != 1.0 or raw_vols.dtype.kind == "f":
            vols = (raw_vols * vol_scale).astype(np.int64)
        else:
            vols = raw_vols.astype(np.int64, copy=False)

        pipeline.ingest_batch(symbols=syms, prices=prices, volumes=vols)
        pipeline.drain()
        ingested += batch.num_rows

    return ingested


# ============================================================================
# Convenience wrappers
# ============================================================================

def ingest_pandas(
    df: "pd.DataFrame",
    pipeline: Any,
    **kwargs,
) -> int:
    """Alias for from_pandas()."""
    return from_pandas(df, pipeline, **kwargs)


def ingest_polars(
    df: "pl.DataFrame",
    pipeline: Any,
    **kwargs,
) -> int:
    """Alias for from_polars()."""
    return from_polars(df, pipeline, **kwargs)


# ============================================================================
# HTTP query result conversion (JSON → DataFrame)
# ============================================================================

def query_to_pandas(json_response: Union[str, dict]) -> "pd.DataFrame":
    """
    Convert ZeptoDB HTTP JSON response to pandas DataFrame.

    Parameters
    ----------
    json_response : str or dict

    Returns
    -------
    pd.DataFrame
    """
    if not HAS_PANDAS:
        raise ImportError("pandas is required")

    import json as _json
    if isinstance(json_response, str):
        data = _json.loads(json_response)
    else:
        data = json_response

    return pd.DataFrame(data.get("data", []), columns=data.get("columns", []))


def query_to_polars(json_response: Union[str, dict]) -> "pl.DataFrame":
    """
    Convert ZeptoDB HTTP JSON response to polars DataFrame.

    Parameters
    ----------
    json_response : str or dict

    Returns
    -------
    pl.DataFrame
    """
    if not HAS_POLARS:
        raise ImportError("polars is required")

    import json as _json
    if isinstance(json_response, str):
        data = _json.loads(json_response)
    else:
        data = json_response

    columns = data.get("columns", [])
    rows = data.get("data", [])

    if not rows:
        return pl.DataFrame(schema=columns)

    return pl.from_records(rows, schema=columns, orient="row")


# ============================================================================
# Internal helpers
# ============================================================================

def _require_cols(container, cols: List[str], label: str) -> None:
    """Raise ValueError if any required column is missing."""
    if HAS_POLARS and isinstance(container, pl.DataFrame):
        available = set(container.columns)
    elif HAS_PYARROW and isinstance(container, pa.Table):
        available = set(container.schema.names)
    elif HAS_PANDAS and isinstance(container, pd.DataFrame):
        available = set(container.columns)
    else:
        available = set(getattr(container, "columns", []))

    missing = [c for c in cols if c not in available]
    if missing:
        raise ValueError(
            f"Missing columns in {label}: {missing}. "
            f"Available: {sorted(available)}. "
            f"Use sym_col/price_col/vol_col to map your column names."
        )


def _arrow_col_to_numpy(col: "pa.Array") -> np.ndarray:
    """Arrow Array → numpy, filling nulls with 0 to avoid cast warnings."""
    if col.null_count > 0:
        import pyarrow.compute as pc
        col = pc.if_else(pc.is_null(col), 0, col)
    return col.to_numpy(zero_copy_only=False)


def _arrow_col_to_int64(col: "pa.Array") -> np.ndarray:
    """Arrow Array → int64 numpy, filling nulls with 0."""
    arr = _arrow_col_to_numpy(col)
    return arr.astype(np.int64, copy=False)

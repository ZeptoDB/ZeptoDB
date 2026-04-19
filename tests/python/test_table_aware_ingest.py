"""
Stage B (devlog 084): Python ingest_batch table_name kwarg round-trip.

Verifies:
  1. `ingest_batch(..., table_name="t1")` lands in t1 (visible via SELECT FROM t1).
  2. Unknown table name raises ValueError.
  3. table_name="" (default) keeps the legacy path — no error, rows ingested.
"""
import os
import sys
import pytest
import numpy as np

# Load the compiled extension from the build dir.
BUILD = os.environ.get("ZEPTODB_BUILD", os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "build")))
sys.path.insert(0, BUILD)
pytest.importorskip("zeptodb")
import zeptodb  # type: ignore


TBL_DDL = (
    "(symbol INT64, price INT64, volume INT64, timestamp TIMESTAMP_NS)"
)


def test_ingest_batch_table_name_lands_in_table():
    p = zeptodb.Pipeline()
    p.start()
    p.execute("CREATE TABLE t1 " + TBL_DDL)

    syms = np.array([1, 1, 1], dtype=np.int64)
    prs  = np.array([100, 101, 102], dtype=np.int64)
    vols = np.array([10, 11, 12], dtype=np.int64)

    p.ingest_batch(symbols=syms, prices=prs, volumes=vols, table_name="t1")
    p.drain()

    r = p.execute("SELECT count(*) FROM t1")
    assert r["ok"], r["error"]
    assert r["data"][0][0] == 3

    # And t2 (no data) sees 0 — strict table-scoped isolation.
    p.execute("CREATE TABLE t2 " + TBL_DDL)
    r2 = p.execute("SELECT count(*) FROM t2")
    assert r2["ok"], r2["error"]
    assert r2["data"][0][0] == 0

    p.stop()


def test_ingest_batch_unknown_table_raises():
    p = zeptodb.Pipeline()
    p.start()

    syms = np.array([1], dtype=np.int64)
    prs  = np.array([100], dtype=np.int64)
    vols = np.array([1], dtype=np.int64)

    with pytest.raises(ValueError):
        p.ingest_batch(symbols=syms, prices=prs, volumes=vols,
                       table_name="nonexistent")

    p.stop()


def test_ingest_batch_empty_table_name_is_legacy():
    # Default kwarg (table_name="") must ingest without error — no CREATE TABLE.
    p = zeptodb.Pipeline()
    p.start()

    syms = np.array([7, 7], dtype=np.int64)
    prs  = np.array([1, 2], dtype=np.int64)
    vols = np.array([1, 1], dtype=np.int64)

    p.ingest_batch(symbols=syms, prices=prs, volumes=vols)  # no table_name
    p.drain()
    assert p.stats()["ticks_stored"] >= 2

    p.stop()


def test_ingest_float_batch_table_name_unknown_raises():
    p = zeptodb.Pipeline()
    p.start()

    syms = np.array([1], dtype=np.int64)
    prs  = np.array([150.25], dtype=np.float64)
    vols = np.array([10.0],   dtype=np.float64)

    with pytest.raises(ValueError):
        p.ingest_float_batch(symbols=syms, prices=prs, volumes=vols,
                             price_scale=100.0, table_name="nonexistent")

    p.stop()


# ============================================================================
# devlog 090: Arrow / Polars DataFrame adapters must honour table_name.
# ============================================================================

def test_from_polars_table_name_lands_in_table():
    pl = pytest.importorskip("polars")
    from zepto_py.dataframe import from_polars

    p = zeptodb.Pipeline()
    p.start()
    p.execute("CREATE TABLE poltab " + TBL_DDL)

    df = pl.DataFrame({
        "sym":    [1, 2, 3],
        "price":  [100, 200, 300],
        "volume": [10, 20, 30],
    })
    n = from_polars(df, p, table_name="poltab")
    assert n == 3
    p.drain()

    r = p.execute("SELECT count(*) FROM poltab")
    assert r["ok"], r["error"]
    assert r["data"][0][0] == 3

    p.stop()


def test_from_arrow_table_name_lands_in_table():
    pa = pytest.importorskip("pyarrow")
    from zepto_py.dataframe import from_arrow

    p = zeptodb.Pipeline()
    p.start()
    p.execute("CREATE TABLE arrtab " + TBL_DDL)

    t = pa.table({
        "sym":    pa.array([1, 2, 3], type=pa.int64()),
        "price":  pa.array([100, 200, 300], type=pa.int64()),
        "volume": pa.array([10, 20, 30], type=pa.int64()),
    })
    n = from_arrow(t, p, table_name="arrtab")
    assert n == 3
    p.drain()

    r = p.execute("SELECT count(*) FROM arrtab")
    assert r["ok"], r["error"]
    assert r["data"][0][0] == 3

    p.stop()

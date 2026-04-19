"""
devlog 088: ZeptoConnection DDL helpers + ingest_pandas(table_name=...).

Verifies the HTTP-client convenience methods (create_table, drop_table,
list_tables) and that `ingest_pandas` honours `table_name` instead of the
previously hard-coded `ticks`.

The tests spawn a local `zepto_http_server` with `--no-auth` on a free port;
if the binary isn't built we skip.
"""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from contextlib import closing

import pytest

# Ensure repo root is on sys.path so `zepto_py` resolves without an install.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from zepto_py.connection import ZeptoConnection


def _free_port() -> int:
    with closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _find_server() -> str | None:
    build = os.environ.get("ZEPTODB_BUILD", os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "build")))
    cand = os.path.join(build, "zepto_http_server")
    return cand if os.path.exists(cand) else None


@pytest.fixture(scope="module")
def db():
    server = _find_server()
    if not server:
        pytest.skip("zepto_http_server binary not built")
    port = _free_port()
    proc = subprocess.Popen(
        [server, "--port", str(port), "--no-auth"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    conn = ZeptoConnection("127.0.0.1", port, timeout=5.0)
    # wait for readiness
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if conn.ping():
            break
        time.sleep(0.1)
    else:
        proc.terminate()
        pytest.skip("zepto_http_server did not start in time")
    yield conn
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def test_create_table_and_list_tables(db):
    db.create_table(
        "test_client_t",
        [("symbol", "INT64"), ("price", "INT64"),
         ("volume", "INT64"), ("timestamp", "INT64")],
        if_not_exists=True,
    )
    tables = db.list_tables()
    assert "test_client_t" in tables
    db.drop_table("test_client_t", if_exists=True)
    assert "test_client_t" not in db.list_tables()


def test_ingest_pandas_with_table_name(db):
    pd = pytest.importorskip("pandas")
    db.create_table(
        "pd_target",
        [("symbol", "INT64"), ("price", "INT64"),
         ("volume", "INT64"), ("timestamp", "INT64")],
        if_not_exists=True,
    )
    df = pd.DataFrame({
        "symbol":    [1, 2, 3],
        "price":     [100, 200, 300],
        "volume":    [10, 20, 30],
        "timestamp": [1, 2, 3],
    })
    n = db.ingest_pandas(df, table_name="pd_target")
    assert n == 3
    rs = db.query("SELECT count(*) FROM pd_target")
    assert rs.rows and rs.rows[0][0] == 3
    db.drop_table("pd_target", if_exists=True)


# ---------------------------------------------------------------------------
# devlog 089 — SQL identifier validation + string escape
# ---------------------------------------------------------------------------


def test_invalid_table_name_rejected(db):
    with pytest.raises(ValueError):
        db.create_table("ticks; DROP TABLE x; --", [("a", "INT64")])


def test_invalid_column_name_rejected(db):
    with pytest.raises(ValueError):
        db.create_table("safe_table", [("col; DROP", "INT64")])


def test_invalid_drop_table_name_rejected(db):
    with pytest.raises(ValueError):
        db.drop_table("ticks; DROP TABLE x; --")


def test_invalid_ingest_pandas_table_name_rejected(db):
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({"symbol": [1], "price": [1], "volume": [1], "timestamp": [1]})
    with pytest.raises(ValueError):
        db.ingest_pandas(df, table_name="bad; DROP")


def test_ingest_pandas_string_with_quotes(db):
    pd = pytest.importorskip("pandas")
    db.create_table(
        "quote_test",
        [("symbol", "SYMBOL"), ("price", "INT64"),
         ("volume", "INT64"), ("timestamp", "INT64")],
        if_not_exists=True,
    )
    df = pd.DataFrame({
        "symbol":    ["it's", "he said \"hi\""],
        "price":     [100, 200],
        "volume":    [10, 20],
        "timestamp": [1, 2],
    })
    n = db.ingest_pandas(df, table_name="quote_test")
    assert n == 2
    db.drop_table("quote_test", if_exists=True)

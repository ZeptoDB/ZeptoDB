"""
ZeptoDB Connection — HTTP client with pandas/polars query results
"""
from __future__ import annotations

import json
import urllib.request
import urllib.error
from typing import Optional, List, Dict, Any, Union
import time

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
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


class QueryResult:
    """Result of a SQL query."""

    def __init__(self, json_data: dict, elapsed_ms: float):
        self._data    = json_data
        self.elapsed_ms = elapsed_ms
        self.columns: List[str] = json_data.get("columns", [])
        self.rows:    List[List] = json_data.get("data", [])
        self.row_count: int = json_data.get("rows", len(self.rows))
        self.execution_time_us: float = json_data.get("execution_time_us", 0)

    def to_pandas(self) -> "pd.DataFrame":
        """Convert result to pandas DataFrame."""
        if not HAS_PANDAS:
            raise ImportError("pandas is required: pip install pandas")
        return pd.DataFrame(self.rows, columns=self.columns)

    def to_polars(self) -> "pl.DataFrame":
        """Convert result to polars DataFrame (zero-copy via Arrow)."""
        if not HAS_POLARS:
            raise ImportError("polars is required: pip install polars")
        return pl.from_records(self.rows, schema=self.columns, orient="row")

    def to_numpy(self) -> Dict[str, "np.ndarray"]:
        """Convert result to dict of numpy arrays."""
        if not HAS_NUMPY:
            raise ImportError("numpy is required: pip install numpy")
        import numpy as np
        if not self.rows:
            return {col: np.array([]) for col in self.columns}

        result = {}
        for i, col in enumerate(self.columns):
            col_data = [row[i] for row in self.rows if i < len(row)]
            result[col] = np.array(col_data)
        return result

    def to_dict(self) -> Dict[str, List]:
        """Convert to column-oriented dict."""
        if not self.rows:
            return {col: [] for col in self.columns}
        result = {col: [] for col in self.columns}
        for row in self.rows:
            for i, col in enumerate(self.columns):
                result[col].append(row[i] if i < len(row) else None)
        return result

    def __repr__(self) -> str:
        return (f"QueryResult(rows={self.row_count}, "
                f"columns={self.columns}, "
                f"elapsed={self.elapsed_ms:.2f}ms)")


class ZeptoConnection:
    """
    Connection to an ZeptoDB server.

    Provides pandas, polars, and numpy integration for query results,
    and batch ingest from DataFrames.

    Examples
    --------
    >>> db = ZeptoConnection("localhost", 8123)
    >>> db.ping()
    True
    >>> df = db.query_pandas("SELECT sym, count(*) FROM trades GROUP BY sym")
    >>> df.head()

    >>> # Ingest from pandas
    >>> import pandas as pd
    >>> ticks = pd.DataFrame({"sym": ["AAPL"]*1000, "price": [150.0]*1000})
    >>> db.ingest_pandas(ticks)
    1000

    >>> # Ingest from polars
    >>> import polars as pl
    >>> ticks_pl = pl.from_pandas(ticks)
    >>> db.ingest_polars(ticks_pl)
    1000
    """

    def __init__(
        self,
        host: str = "localhost",
        port: int = 8123,
        timeout: float = 30.0,
        database: str = "default",
    ):
        self.host    = host
        self.port    = port
        self.timeout = timeout
        self.database = database
        self._base_url = f"http://{host}:{port}"

    # ------------------------------------------------------------------
    # SQL identifier validation (devlog 089 — injection guard)
    # ------------------------------------------------------------------

    @staticmethod
    def _validate_identifier(name: str) -> str:
        """Validate SQL identifier (table/column name).

        Raises ``ValueError`` if the name doesn't match
        ``^[A-Za-z_][A-Za-z0-9_]*$``. Applied to every caller-supplied
        table/column name before it's interpolated into a SQL string.
        """
        import re
        if not isinstance(name, str) or not re.match(
                r'^[A-Za-z_][A-Za-z0-9_]*$', name):
            raise ValueError(f"Invalid SQL identifier: {name!r}")
        return name

    @staticmethod
    def _validate_type(name: str) -> str:
        """Validate SQL column type string (e.g. ``INT64``, ``DOUBLE``).

        Types are a controlled set but we still reject anything outside
        ``^[A-Za-z0-9_]+$`` to prevent injection via a misused kwarg.
        """
        import re
        if not isinstance(name, str) or not re.match(
                r'^[A-Za-z0-9_]+$', name):
            raise ValueError(f"Invalid SQL type: {name!r}")
        return name

    # ------------------------------------------------------------------
    # Core HTTP methods
    # ------------------------------------------------------------------

    def _post(self, query: str) -> tuple[dict, float]:
        """Execute SQL via HTTP POST, return (parsed_json, elapsed_ms)."""
        url  = self._base_url + "/"
        data = query.encode("utf-8")
        req  = urllib.request.Request(
            url,
            data=data,
            method="POST",
            headers={"Content-Type": "text/plain"},
        )
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8")
            raise RuntimeError(f"ZeptoDB HTTP {e.code}: {body}") from e
        except urllib.error.URLError as e:
            raise ConnectionError(
                f"Cannot connect to {self.host}:{self.port} — {e.reason}"
            ) from e
        elapsed_ms = (time.perf_counter() - t0) * 1000

        try:
            return json.loads(body), elapsed_ms
        except json.JSONDecodeError:
            return {"columns": [], "data": [], "raw": body}, elapsed_ms

    def _get(self, path: str) -> str:
        url = self._base_url + path
        try:
            with urllib.request.urlopen(url, timeout=self.timeout) as resp:
                return resp.read().decode("utf-8")
        except Exception as e:
            return str(e)

    # ------------------------------------------------------------------
    # Connectivity
    # ------------------------------------------------------------------

    def ping(self) -> bool:
        """Check server connectivity."""
        try:
            result = self._get("/ping")
            return "ok" in result.lower() or result.startswith("{")
        except Exception:
            return False

    def stats(self) -> dict:
        """Get server statistics."""
        try:
            body = self._get("/stats")
            return json.loads(body)
        except Exception:
            return {}

    def health(self) -> dict:
        """Get server health status."""
        try:
            body = self._get("/health")
            return json.loads(body)
        except Exception:
            return {}

    # ------------------------------------------------------------------
    # Query methods
    # ------------------------------------------------------------------

    def query(self, sql: str) -> QueryResult:
        """Execute SQL and return a QueryResult."""
        data, elapsed = self._post(sql)
        return QueryResult(data, elapsed)

    def query_pandas(self, sql: str) -> "pd.DataFrame":
        """Execute SQL and return a pandas DataFrame."""
        if not HAS_PANDAS:
            raise ImportError("pandas is required: pip install pandas")
        return self.query(sql).to_pandas()

    def query_polars(self, sql: str) -> "pl.DataFrame":
        """Execute SQL and return a polars DataFrame."""
        if not HAS_POLARS:
            raise ImportError("polars is required: pip install polars")
        return self.query(sql).to_polars()

    def query_numpy(self, sql: str) -> Dict[str, "np.ndarray"]:
        """Execute SQL and return dict of numpy arrays."""
        return self.query(sql).to_numpy()

    def execute(self, sql: str) -> int:
        """Execute DDL/DML, return affected rows."""
        result = self.query(sql)
        return result.row_count

    # ------------------------------------------------------------------
    # DDL convenience helpers
    # ------------------------------------------------------------------

    def create_table(
        self,
        name: str,
        columns: "list[tuple[str, str]]",
        if_not_exists: bool = False,
    ) -> None:
        """Create a table. columns: [("symbol", "INT64"), ("price", "INT64"), ...]"""
        self._validate_identifier(name)
        cols_sql = ", ".join(
            f"{self._validate_identifier(n)} {self._validate_type(t)}"
            for n, t in columns
        )
        ine = "IF NOT EXISTS " if if_not_exists else ""
        self.execute(f"CREATE TABLE {ine}{name} ({cols_sql})")

    def drop_table(self, name: str, if_exists: bool = False) -> None:
        """Drop a table."""
        self._validate_identifier(name)
        ie = "IF EXISTS " if if_exists else ""
        self.execute(f"DROP TABLE {ie}{name}")

    def list_tables(self) -> "list[str]":
        """Return list of table names via SHOW TABLES."""
        rs = self.query("SHOW TABLES")
        # SHOW TABLES returns one column with table names
        return [row[0] for row in rs.rows] if rs.rows else []

    # ------------------------------------------------------------------
    # Pandas ingest
    # ------------------------------------------------------------------

    def ingest_pandas(
        self,
        df: "pd.DataFrame",
        batch_size: int = 10_000,
        sym_col: str = "sym",
        timestamp_col: Optional[str] = "timestamp",
        show_progress: bool = False,
        table_name: str = "ticks",
    ) -> int:
        """
        Ingest a pandas DataFrame into ZeptoDB.

        Converts each row to an INSERT statement (batch_size rows at a time).
        For high-throughput use, prefer the native C++ pybind11 API.

        Parameters
        ----------
        df : pd.DataFrame
        batch_size : int, default 10_000
        sym_col : str, column containing symbol/key
        timestamp_col : str or None
        show_progress : bool
        table_name : str, default "ticks"
            Destination table for the generated INSERT statements.

        Returns
        -------
        int : number of rows ingested
        """
        if not HAS_PANDAS:
            raise ImportError("pandas is required")

        self._validate_identifier(table_name)

        cols = list(df.columns)
        total = len(df)
        ingested = 0

        for start in range(0, total, batch_size):
            chunk = df.iloc[start : start + batch_size]
            values = []
            for _, row in chunk.iterrows():
                row_vals = []
                for col in cols:
                    val = row[col]
                    if isinstance(val, str):
                        # devlog 089: double embedded single quotes (SQL escape)
                        row_vals.append("'" + val.replace("'", "''") + "'")
                    elif hasattr(val, 'item'):  # numpy scalar
                        row_vals.append(str(val.item()))
                    else:
                        row_vals.append(str(val))
                values.append(f"({', '.join(row_vals)})")

            sql = (
                f"INSERT INTO {table_name} ({', '.join(cols)}) VALUES "
                + ", ".join(values)
            )
            self._post(sql)
            ingested += len(chunk)

            if show_progress:
                print(f"\rIngested {ingested}/{total}", end="", flush=True)

        if show_progress:
            print()

        return ingested

    def ingest_pandas_fast(
        self,
        df: "pd.DataFrame",
        sym_col: str = "sym",
    ) -> int:
        """
        Fast ingest using the pipeline API (column-wise, minimal overhead).
        Requires the zepto_py C++ extension to be loaded.
        """
        try:
            import zeptodb  # C++ pybind11 module
            pipeline = zeptodb.Pipeline()
            from .streaming import StreamingSession
            sess = StreamingSession(pipeline)
            return sess.ingest_pandas(df)
        except ImportError:
            return self.ingest_pandas(df, sym_col=sym_col)

    # ------------------------------------------------------------------
    # Polars ingest
    # ------------------------------------------------------------------

    def ingest_polars(
        self,
        df: "pl.DataFrame",
        batch_size: int = 10_000,
        show_progress: bool = False,
        table_name: str = "ticks",
    ) -> int:
        """
        Ingest a polars DataFrame into ZeptoDB.
        Converts to pandas internally for batch ingest.
        """
        if not HAS_POLARS:
            raise ImportError("polars is required")
        self._validate_identifier(table_name)
        pd_df = df.to_pandas()
        return self.ingest_pandas(pd_df, batch_size=batch_size,
                                  show_progress=show_progress,
                                  table_name=table_name)

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self) -> "ZeptoConnection":
        return self

    def __exit__(self, *args) -> None:
        pass

    def __repr__(self) -> str:
        return f"ZeptoConnection(host={self.host!r}, port={self.port})"


def connect(
    host: str = "localhost",
    port: int = 8123,
    timeout: float = 30.0,
    database: str = "default",
) -> ZeptoConnection:
    """
    Connect to an ZeptoDB server.

    Parameters
    ----------
    host : str, default "localhost"
    port : int, default 8123
    timeout : float, default 30.0
    database : str, default "default"

    Returns
    -------
    ZeptoConnection

    Examples
    --------
    >>> db = zepto_py.connect("localhost", 8123)
    >>> db.ping()
    True
    """
    return ZeptoConnection(host=host, port=port, timeout=timeout, database=database)

"""
ZeptoDB Python Client
=====================
Ultra-low latency time-series database with pandas/polars/arrow support.

Quick Start
-----------
>>> import zepto_py as zepto
>>> db = zepto.connect("localhost", 8123)
>>>
>>> # Ingest from pandas
>>> import pandas as pd
>>> df = pd.DataFrame({"sym": ["AAPL"], "price": [150.0], "size": [100]})
>>> db.ingest_pandas(df)
>>>
>>> # Query → pandas
>>> result = db.query_pandas("SELECT sym, avg(price) FROM trades GROUP BY sym")
>>>
>>> # Query → polars
>>> result = db.query_polars("SELECT * FROM trades WHERE sym='AAPL' LIMIT 1000")

Cluster routing (in-process pybind11 pipeline)
-----------------------------------------------
For in-process workloads that participate in a ZeptoDB cluster, the low-level
``zeptodb.Pipeline`` exposes ``enable_cluster_routing`` (devlog 114). INSERT
and ingest calls are then dispatched via the consistent-hash ring instead of
landing on whichever pod the request hit:

>>> import zeptodb
>>> p = zeptodb.Pipeline()
>>> p.start()
>>> p.enable_cluster_routing(
...     self_id=0,
...     peers=[(1, "storage-1", 8123), (2, "storage-2", 8123)],
...     remove_self_from_ring=False,     # full cluster node; keep self in ring
...     rpc_timeout_ms=2000,
... )

See ``docs/api/PYTHON_REFERENCE.md`` for the full signature. ``zepto_py`` (the
HTTP client package) does not need a corresponding helper — cluster routing
is handled server-side by ``zepto_http_server`` once the binary is in cluster
mode (devlog 111).
"""

from .connection import AgentCacheClient, AgentMemoryClient, ZeptoConnection, connect
from .dataframe import (
    from_pandas,
    from_polars,
    from_polars_arrow,
    from_arrow,
    to_pandas,
    to_polars,
    ingest_pandas,
    ingest_polars,
)
from .streaming import StreamingSession
from .arrow import ArrowSession
from .utils import check_dependencies, versions

__version__ = "0.1.6"
__all__ = [
    "connect",
    "ZeptoConnection",
    "AgentMemoryClient",
    "AgentCacheClient",
    "from_pandas",
    "from_polars",
    "from_polars_arrow",
    "from_arrow",
    "to_pandas",
    "to_polars",
    "ingest_pandas",
    "ingest_polars",
    "StreamingSession",
    "ArrowSession",
    "check_dependencies",
    "versions",
]

"""
APEX-DB Python Client
=====================
Ultra-low latency time-series database with pandas/polars/arrow support.

Quick Start
-----------
>>> import apex_py as apex
>>> db = apex.connect("localhost", 8123)
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
"""

from .connection import ApexConnection, connect
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

__version__ = "0.1.0"
__all__ = [
    "connect",
    "ApexConnection",
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

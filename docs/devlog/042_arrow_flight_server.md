# Devlog 038: Arrow Flight Server (P3)

**Date:** 2026-03-31
**Status:** ✅ Complete

## Summary

Arrow Flight RPC server for ZeptoDB. Enables remote Python/Polars/Pandas
clients to stream query results via `flight://` protocol with near-zero-copy
efficiency (Arrow IPC over gRPC).

## Motivation

- Current zero-copy path only works for C++ embedded or local Python binding
- Arrow Flight gives remote Python quants the same columnar streaming
- Standard protocol: `pyarrow.flight`, Polars, DuckDB, Spark all speak it

## Design

### Architecture

```
Python Client                    ZeptoDB
─────────────                    ───────
pyarrow.flight.connect()  ──→  FlightServer (gRPC :8815)
  DoGet(Ticket="SQL")     ──→    QueryExecutor.execute(sql)
  ←── RecordBatchStream   ←──    QueryResultSet → Arrow RecordBatch
```

### Implemented RPCs

| RPC | Purpose |
|-----|---------|
| `GetFlightInfo` | Schema + row count for a SQL query |
| `DoGet` | Execute SQL, stream results as Arrow RecordBatches |
| `DoPut` | Ingest Arrow RecordBatches into a table |
| `ListFlights` | List available tables |
| `DoAction` | "ping", "healthcheck" |
| `ListActions` | List supported actions |

### Type Mapping

| ZeptoDB ColumnType | Arrow Type |
|-------------------|------------|
| INT64 | int64 |
| FLOAT32 | float32 |
| FLOAT64 | float64 |
| STRING | utf8 |

### Files

| File | Purpose |
|------|---------|
| `include/zeptodb/server/flight_server.h` | Public header |
| `src/server/flight_server.cpp` | Implementation (FlightServerBase subclass) |
| `tools/zepto_flight_server.cpp` | CLI binary (HTTP + Flight dual server) |
| `tests/unit/test_flight_server.cpp` | 7 tests (DoGet, GetFlightInfo, DoAction, etc.) |

### Build

```bash
# Auto-detected from pyarrow bundle
cmake .. -G Ninja -DZEPTO_USE_FLIGHT=ON
ninja zepto_flight_server

# Run
LD_LIBRARY_PATH=$(python3 -c "import pyarrow; print(pyarrow.get_library_dirs()[0])"):$LD_LIBRARY_PATH \
  ./zepto_flight_server --flight-port 8815 --http-port 8123
```

### Python Client Example

```python
import pyarrow.flight as fl

client = fl.connect("grpc://localhost:8815")

# Query
reader = client.do_get(fl.Ticket("SELECT * FROM trades LIMIT 10"))
df = reader.read_all().to_pandas()
print(df)

# Health check
results = list(client.do_action(fl.Action("ping")))
print(results[0].body.to_pybytes())
```

## Tests

7 new tests, all passing:
- `DoGetSelectCount` — COUNT(*) returns correct value
- `DoGetSelectAll` — SELECT * returns correct columns/rows
- `GetFlightInfo` — schema + row count metadata
- `DoActionPing` — health check action
- `ListActions` — lists available actions
- `DoGetInvalidSQL` — graceful error handling
- `ServerStartStop` — lifecycle management

## Stub Mode

When built without Arrow Flight (`-DZEPTO_USE_FLIGHT=OFF`), all methods
are no-ops. The `FlightServerStub` test verifies compilation.

# Arrow Flight API Reference

ZeptoDB exposes an [Arrow Flight](https://arrow.apache.org/docs/format/Flight.html) RPC server for high-throughput columnar data streaming.

**Default port:** `8815` (gRPC)

---

## Connection

```python
import pyarrow.flight as fl

client = fl.connect("grpc://localhost:8815")
```

---

## RPCs

### DoGet — Execute SQL Query

Send a SQL string as a `Ticket`, receive results as Arrow RecordBatches.

```python
reader = client.do_get(fl.Ticket("SELECT * FROM trades WHERE symbol = 'AAPL' LIMIT 100"))
table = reader.read_all()          # pyarrow.Table
df = table.to_pandas()             # pandas DataFrame
pl_df = polars.from_arrow(table)   # polars DataFrame
```

### GetFlightInfo — Query Metadata

Get schema and row count without fetching data.

```python
desc = fl.FlightDescriptor.for_command("SELECT COUNT(*) FROM trades")
info = client.get_flight_info(desc)
print(info.total_records)   # row count
print(info.schema)          # Arrow schema
```

### DoPut — Ingest Data

Upload Arrow RecordBatches into a table.

```python
import pyarrow as pa

schema = pa.schema([
    ("symbol", pa.int64()),
    ("price", pa.int64()),
    ("volume", pa.int64()),
    ("timestamp", pa.int64()),
])
batch = pa.record_batch([
    [1, 1, 2],
    [15000000000, 15100000000, 15200000000],
    [100, 200, 150],
    [1711234567000000000, 1711234568000000000, 1711234569000000000],
], schema=schema)

desc = fl.FlightDescriptor.for_command("trades")
writer, _ = client.do_put(desc, schema)
writer.write_batch(batch)
writer.close()
```

### ListFlights — List Tables

```python
for info in client.list_flights():
    print(info.descriptor.command, info.total_records)
```

### DoAction — Health Check

```python
results = list(client.do_action(fl.Action("ping")))
print(results[0].body.to_pybytes())  # b'{"status":"ok"}'
```

### ListActions

```python
for action in client.list_actions():
    print(action.type, action.description)
# ping       Health check
# healthcheck Health check (alias)
```

---

## Type Mapping

| ZeptoDB Type | Arrow Type | Notes |
|-------------|------------|-------|
| INT64 | `int64` | Default numeric type |
| FLOAT32 | `float32` | IEEE 754 single |
| FLOAT64 | `float64` | IEEE 754 double |
| STRING/SYMBOL | `utf8` | Dictionary-decoded to string |

---

## CLI Server

```bash
# Start both HTTP and Flight servers
./zepto_flight_server --http-port 8123 --flight-port 8815 --ticks 10000

# Flight-only (HTTP still starts for admin)
./zepto_flight_server --flight-port 8815 --no-auth
```

---

## Build

Arrow Flight is auto-detected from pyarrow:

```bash
pip3 install pyarrow
cmake .. -G Ninja -DZEPTO_USE_FLIGHT=ON   # ON by default
ninja zepto_flight_server
```

Disable: `-DZEPTO_USE_FLIGHT=OFF` (stub mode, all methods are no-ops).

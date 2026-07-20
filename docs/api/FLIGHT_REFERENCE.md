# Arrow Flight API Reference

ZeptoDB exposes an [Arrow Flight](https://arrow.apache.org/docs/format/Flight.html)
RPC server for columnar query results. Batch ingestion remains an explicit
experimental path because stream-level atomic commit is not implemented.

**Default address:** `127.0.0.1:8815`

The default listener is intentionally loopback-only. A non-loopback listener
requires TLS unless the operator supplies the explicit development-only
`--allow-insecure-flight` override.

## Authentication

Protected RPCs use the same `AuthManager` API-key/JWT validation, rate limits,
roles, table allowlists, and tenant identity as the HTTP server. Send
`authorization: Bearer <credential>` as per-RPC Flight metadata:

```python
import pyarrow.flight as fl

client = fl.connect("grpc://127.0.0.1:8815")
call_options = fl.FlightCallOptions(headers=[
    (b"authorization", b"Bearer zepto_example_key"),
])

ticket = fl.Ticket("SELECT * FROM trades LIMIT 100")
table = client.do_get(ticket, call_options).read_all()
```

Pass `call_options` to every protected RPC. Do not use connection-level
`generic_options` for the bearer credential; those are gRPC transport options,
not Flight request metadata.

| RPC | Required permission | Additional policy |
| --- | --- | --- |
| `GetFlightInfo` | `READ` | Read-only SQL; recursive table and tenant checks |
| `DoGet` | `READ` | Read-only SQL; recursive table and tenant checks |
| `DoPut` | `WRITE` | Disabled by default; experimental non-atomic opt-in, descriptor table allowlist and tenant check |
| `ListFlights` | `READ` | Results filtered by table allowlist and tenant |
| `DoAction(ping)` | Public | Health response only |
| `DoAction(healthcheck)` | Public | Health response only |
| `ListActions` | Public | Lists public health actions |

Missing or invalid credentials return Flight `Unauthenticated` (HTTP-equivalent
401). Valid credentials that fail role, rate, table, or tenant policy return
Flight `Unauthorized` (HTTP-equivalent 403). Caller-supplied `x-zepto-role`,
`x-zepto-allowed-tables`, and similar metadata are ignored; authorization comes
only from the validated `AuthContext`.

An embedding that constructs `FlightServer` without an `AuthManager`, or with
an explicitly disabled manager, retains local development compatibility.
The CLI requires an explicit credential source through `--api-keys-file` or
`ZEPTO_API_KEYS_FILE`, and refuses to bind if the store has no active key.
`--bootstrap-dev-keys` creates and prints local development keys only when
explicitly requested. `--no-auth` remains a separate development override.

## Query RPCs

### DoGet

Send a SQL string as a `Ticket` and receive Arrow record batches:

```python
reader = client.do_get(
    fl.Ticket("SELECT * FROM trades WHERE symbol = 'AAPL' LIMIT 100"),
    call_options,
)
table = reader.read_all()
df = table.to_pandas()
```

Flight query tickets accept only structurally valid `SELECT`, `SHOW TABLES`,
and `DESCRIBE` statements. DML and DDL are rejected even for an administrator.
This prevents `GetFlightInfo` followed by `DoGet` from executing a mutation
twice. Malformed or unclassified SQL fails closed.

Read RPCs execute the request-owned, policy-bounded AST directly rather than
through shared prepared/result caches. This prevents cache priming, concurrent
cap replacement, hash collisions, or cache exhaustion from changing the
authorized statement. The default cooperative timeout is 30 seconds
(`--query-timeout-ms`; `0` disables it). Tenant-scoped identities consume their
configured `max_concurrent_queries` slot for the lifetime of each RPC; quota
exhaustion returns Flight `Unavailable`.

For `SELECT`, ZeptoDB checks every physical table reached through JOINs, CTEs,
subqueries, scalar/`IN` subqueries, and set operations. CTE visibility is
evaluated in declaration order so a later CTE cannot hide an earlier physical
table from authorization. `SHOW TABLES` output is filtered to visible tables.

### GetFlightInfo

`GetFlightInfo` executes the read-only query to determine its schema and row
count, then returns a ticket for `DoGet`:

```python
descriptor = fl.FlightDescriptor.for_command(
    "SELECT COUNT(*) FROM trades"
)
info = client.get_flight_info(descriptor, call_options)
print(info.total_records)
print(info.schema)
```

Calling `DoGet` on the returned ticket executes the query again. Only read-only
statements are accepted on both RPCs.

## Ingestion RPC

### DoPut

`DoPut` is disabled by default. The current compatibility implementation
commits one row at a time and cannot roll back a partially accepted Flight
stream. It may be enabled for an isolated experiment with
`--allow-non-atomic-put`; startup prints a warning. Do not enable this flag for
production ingestion.

The command descriptor is a table name, not SQL. The caller needs `WRITE`
permission and access to that table under both the identity table allowlist and
tenant namespace policy.

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

descriptor = fl.FlightDescriptor.for_command("trades")
writer, _ = client.do_put(descriptor, schema, call_options)
writer.write_batch(batch)
writer.close()
```

Accepted Arrow inputs are `int64`, `float32`, `float64`, and UTF-8 string.
NULLs, non-finite floats, unsupported types, and non-identifier table
descriptors are rejected. String values are SQL-escaped before ingestion. A
stream is not transactional: rows accepted before a later row or batch error
remain committed, so retry policy must account for possible partial writes.

Default per-RPC limits are 100,000 rows and 64 MiB for query results, and
100,000 cumulative rows and 64 MiB of Arrow input for one `DoPut`. A batch that
would cross a `DoPut` limit is rejected before that batch is ingested, but
earlier accepted batches remain committed. Query byte size is checked after
materialization and Arrow encoding, so this is a response cap rather than a
streaming executor memory guarantee.

The non-atomic opt-in is an **Experimental runtime path**. Its intended scope is
bounded compatibility testing with idempotent disposable input. Non-goals are
exactly-once ingestion, rollback, crash recovery, and safe blind retry. Disable
it by removing the flag. Product promotion requires an atomic stream commit
primitive, fault/retry tests, durable recovery evidence, and ingest telemetry.

## Discovery and Health

`ListFlights` requires `READ` and returns only tables allowed by both table and
tenant policy:

```python
for info in client.list_flights(options=call_options):
    print(info.descriptor.command, info.total_records)
```

Health actions are intentionally public:

```python
results = list(client.do_action(fl.Action("ping")))
print(results[0].body.to_pybytes())  # b'{"status":"ok"}'

for action in client.list_actions():
    print(action.type, action.description)
```

## Type Mapping

| ZeptoDB type | Arrow type | Notes |
| --- | --- | --- |
| `INT64` | `int64` | Default numeric type |
| `FLOAT32` | `float32` | IEEE 754 single |
| `FLOAT64` | `float64` | IEEE 754 double |
| `STRING` / `SYMBOL` | `utf8` | Dictionary-decoded for results |

## TLS and Listener Policy

Production listeners use a PEM certificate/key pair and a non-loopback bind:

```bash
./zepto_flight_server \
  --flight-host 0.0.0.0 \
  --flight-port 8815 \
  --tls-cert /etc/zeptodb/tls/server.crt \
  --tls-key /etc/zeptodb/tls/server.key \
  --api-keys-file /etc/zeptodb/api_keys.txt
```

`--port` is an alias for `--flight-port`. The certificate and key must both be
present, readable, non-empty PEM files. The CLI also applies the pair to its
bundled HTTP server so bearer credentials are not sent over plaintext there.

```python
client = fl.connect(
    "grpc+tls://zepto.internal:8815",
    tls_root_certs=open("ca.crt", "rb").read(),
)
```

For local development, the safe default works without TLS:

```bash
./zepto_flight_server --flight-port 8815 --api-keys-file ./dev_keys.txt
```

Plaintext `0.0.0.0`/external binds fail startup. The override below is
development-only and emits a warning:

```bash
./zepto_flight_server \
  --flight-host 0.0.0.0 \
  --allow-insecure-flight \
  --allow-plaintext-http \
  --no-auth
```

The two plaintext overrides are intentionally independent: the Flight override
never exposes the bundled HTTP listener by itself.

## C++ Embedding

```cpp
zeptodb::server::FlightServer server(executor, auth, tenant_manager);
zeptodb::server::FlightServerConfig config;
config.host = "0.0.0.0";
config.port = 8815;
config.tls_cert_path = "/etc/zeptodb/tls/server.crt";
config.tls_key_path = "/etc/zeptodb/tls/server.key";
config.max_query_rows = 100000;
config.max_query_bytes = 64U * 1024U * 1024U;
config.query_timeout_ms = 30000;
config.max_put_rows = 100000;
config.max_put_bytes = 64U * 1024U * 1024U;
config.allow_non_atomic_put = false;

if (!server.start_async(config)) {
    throw std::runtime_error(server.last_error());
}
```

Tenant-scoped credentials fail closed when the embedding has not supplied a
matching `TenantManager` configuration.

## Build

Arrow Flight is auto-detected from a system Arrow installation or pyarrow:

```bash
pip3 install pyarrow
cmake .. -G Ninja -DZEPTO_USE_FLIGHT=ON
ninja zepto_flight_server
```

Use `-DZEPTO_USE_FLIGHT=OFF` for a build without the Flight transport. In that
configuration the lifecycle methods remain stubbed; the config-taking startup
overloads return `false` and expose an unavailable-build message through
`last_error()`.

# Telegraf Output Integration

ZeptoDB ships `zepto-telegraf-output`, an external Telegraf output program for
`outputs.execd`. It reads Telegraf's Influx line protocol from `stdin`, maps
each metric into ZeptoDB tick columns, and writes batched SQL `INSERT` requests
to the ZeptoDB HTTP endpoint.

This keeps the integration independent of Telegraf release cadence while still
unlocking Telegraf's input plugin ecosystem.

Telegraf reference:
[`outputs.execd`](https://docs.influxdata.com/telegraf/v1/output-plugins/execd/)
and
[`external plugins`](https://docs.influxdata.com/telegraf/v1/configure_plugins/external_plugins/).

## Build

```bash
cd build
ninja -j$(nproc) zepto-telegraf-output
```

## ZeptoDB Table

Create a destination table before starting Telegraf:

```bash
curl -X POST http://localhost:8123/ \
  -d "CREATE TABLE telegraf (symbol SYMBOL, price INT64, volume INT64, timestamp INT64)"
```

The default tool mapping writes:

| ZeptoDB column | Source |
|---|---|
| `symbol` | Telegraf tag named `symbol`; falls back to measurement name |
| `price` | Numeric field named `value` |
| `volume` | Numeric field named `volume`, or `1` when absent |
| `timestamp` | Line protocol timestamp, interpreted as nanoseconds by default |

Use `--price-field`, `--volume-field`, `--symbol-tag`, and
`--timestamp-unit` when your Telegraf input uses different names or timestamp
precision.

## Telegraf Configuration

Example with `outputs.execd`:

```toml
[[outputs.execd]]
  command = [
    "/opt/zeptodb/bin/zepto-telegraf-output",
    "--url", "http://127.0.0.1:8123",
    "--table", "telegraf",
    "--symbol-tag", "host",
    "--price-field", "usage_idle",
    "--timestamp-unit", "ns"
  ]
  data_format = "influx"
  influx_timestamp_precision = "1ns"
```

For authenticated ZeptoDB deployments, pass an API key through the environment:

```bash
export ZEPTO_TELEGRAF_TOKEN="zepto_..."
```

The tool also accepts `--auth-token TOKEN`, `ZEPTO_API_KEY`,
`ZEPTO_TELEGRAF_URL`, and `ZEPTO_TELEGRAF_TABLE`.

For no-auth tenant-scoped deployments, pass:

```toml
  command = [
    "/opt/zeptodb/bin/zepto-telegraf-output",
    "--tenant", "tenant_a"
  ]
```

## CLI Options

```bash
zepto-telegraf-output --help
```

Important options:

| Option | Default | Meaning |
|---|---|---|
| `--url` | `http://127.0.0.1:8123` | ZeptoDB HTTP endpoint |
| `--table` | `telegraf` | Destination table |
| `--symbol-tag` | `symbol` | Tag used as ZeptoDB symbol |
| `--no-measurement-symbol` | off | Reject rows missing `--symbol-tag` instead of using measurement name |
| `--price-field` | `value` | Numeric field mapped to `price` |
| `--volume-field` | `volume` | Numeric field mapped to `volume` |
| `--default-volume` | `1` | Volume when `--volume-field` is absent |
| `--price-scale` | `1` | Multiplier before int64 price storage |
| `--volume-scale` | `1` | Multiplier before int64 volume storage |
| `--timestamp-unit` | `ns` | `ns`, `us`, `ms`, or `s` |
| `--batch-size` | `1` | Metrics per SQL INSERT request |
| `--fail-on-parse-error` | off | Exit non-zero instead of dropping malformed metrics |

## Smoke Test

Run ZeptoDB:

```bash
./build/zepto_http_server --port 8123 --no-auth
```

Create the table:

```bash
curl -X POST http://localhost:8123/ \
  -d "CREATE TABLE telegraf (symbol SYMBOL, price INT64, volume INT64, timestamp INT64)"
```

Send one Telegraf line:

```bash
printf 'cpu,host=edge01 usage_idle=99.5,volume=1i 1711234567000000000\n' \
  | ./build/zepto-telegraf-output \
      --table telegraf \
      --symbol-tag host \
      --price-field usage_idle \
      --price-scale 100
```

Query it:

```bash
curl -X POST http://localhost:8123/ \
  -d "SELECT symbol, price, volume FROM telegraf WHERE symbol = 'edge01'"
```

Expected price is `9950` because `99.5 * --price-scale 100` is stored as an
integer.

## Notes

- Default `--batch-size 1` favors delivery latency and avoids metrics waiting
  indefinitely in a long-running `outputs.execd` process. Increase it for high
  volume inputs once you are comfortable with the flush behavior.
- The current writer uses SQL `INSERT` over HTTP. `POST /insert/msgpack` is
  available as the binary follow-on transport without changing the
  Telegraf-side config shape.
- Table names are accepted only as simple SQL identifiers
  `[A-Za-z_][A-Za-z0-9_]*` to avoid SQL injection through plugin config.

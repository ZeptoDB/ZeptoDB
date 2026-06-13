# 160: P5 Telegraf Output Plugin

Date: 2026-06-03
Status: Complete

## Context

P5 tracks ecosystem data pipelines. The highest-leverage open item was a
Telegraf output plugin because it lets ZeptoDB ingest from Telegraf's broad
input ecosystem without adding a custom source connector for every sensor,
system metric, or industrial device protocol.

Telegraf supports external output programs through `outputs.execd`, so the
smallest complete ZeptoDB surface is an external writer binary rather than a
Go plugin vendored into Telegraf.

## Changes

- Added `include/zeptodb/feeds/telegraf_output.h` and
  `src/feeds/telegraf_output.cpp` for Influx line protocol parsing, metric →
  `(symbol, price, volume, timestamp)` mapping, safe table-name validation,
  SQL string escaping, timestamp unit conversion, and multi-row SQL INSERT
  generation.
- Added `tools/zepto-telegraf-output.cpp`, a standalone `outputs.execd`
  program that reads line protocol from `stdin` and writes ZeptoDB HTTP SQL
  INSERT requests. It supports destination table, auth token, tenant header,
  symbol tag, price/volume field names, scales, timestamp unit, batch size,
  and fail-on-parse-error behavior.
- Added `zepto_telegraf_output` and `zepto-telegraf-output` CMake targets.
- Added `tests/unit/test_telegraf_output.cpp` for happy path parsing,
  escaped measurement/tag/string handling, malformed input, scaled numeric
  mapping, measurement fallback, missing/non-numeric price rejection, SQL
  escaping, unsafe table-name rejection, timestamp unit parsing, and timestamp
  scale overflow.
- Added `docs/operations/TELEGRAF_OUTPUT.md` with a Telegraf `outputs.execd`
  configuration and smoke test.
- Updated P5 backlog/completed docs. The stale MQTT backlog row was also
  removed because MQTT was already completed in devlog 081 and listed in
  `docs/COMPLETED.md`.

## Verification

- `cmake --build build --target zepto_telegraf_output zepto-telegraf-output zepto_tests -j$(nproc)`
- `./build/tests/zepto_tests --gtest_filter='TelegrafOutputTest.*'` — 10/10 passed
- `./build/zepto-telegraf-output --help`
- HTTP smoke on port 18421: created `telegraf`, piped one line through
  `zepto-telegraf-output`, and verified `SELECT count(*) FROM telegraf WHERE
  symbol = 'edge01'` returned `1`.
- Full local verification after the P9 closeout branch was assembled:
  - `./build/tests/zepto_tests` — 1441 run, 1440 passed, 1 live S3 upload
    skipped because `ZEPTO_S3_TEST_BUCKET` was unset.
  - `cmake --build build -j$(nproc)`
  - `python3 -m pytest -v tests/python` — 234 passed, 1 warning.

Cross-architecture verification was not rerun for this narrow P5 slice.

## Follow-ups

- P4 MessagePack ingest can replace the current SQL-over-HTTP transport for
  higher-throughput Telegraf writes without changing the Telegraf config shape.
- Remaining P5 items: Kafka Connect Sink, Debezium CDC connector, AWS Kinesis
  consumer, and Apache Pulsar consumer.

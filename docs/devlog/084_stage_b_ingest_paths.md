# Devlog 084 — Stage B: Table-Aware Ingest Paths & Migration Tools

Date: 2026-04-18
Status: Complete (Stage B of the 8-limitations closure from devlog 082)
Related: `docs/devlog/083_stage_a_hdb_and_durability.md`, `docs/design/layer2_ingestion_network.md`, `docs/design/layer4_transpiler_client.md`, `docs/api/PYTHON_REFERENCE.md`

## Scope (Stage B only)

Stage A (devlog 083) made HDB + SchemaRegistry table-aware on the storage side. Stage B now makes every **ingress** surface table-aware so that every path that creates a `TickMessage` can stamp `table_id` before it enters the pipeline:

1. Python binding — `Pipeline.ingest_batch(..., table_name="t")` and `ingest_float_batch(..., table_name="t")`.
2. Python DSL — `from_pandas / from_polars / from_polars_arrow / from_arrow` / `ArrowSession.ingest_arrow[_columnar]` thread `table_name` through to the C++ layer.
3. Feed handlers — Kafka and MQTT consumers accept `KafkaConfig::table_name` / `MqttConfig::table_name`, resolve the id once via `SchemaRegistry`, and stamp it on every decoded tick. FIX / NASDAQ ITCH / Binance handler classes gain `set_table_name / set_table_id` setters for upstream use.
4. Migration tools — `ClickHouseMigrator::Config`, `DuckDBIntegrator::Config`, `TimescaleDBMigrator::Config`, and `HDBLoader` all gained a `dest_table` field; `zepto-migrate` wires it through a new `--dest-table <name>` CLI flag.
5. WAL forward-compat note — the WAL already serializes the full 64-byte `TickMessage`, so `table_id` is already carried. No WAL format change; replay from v1 WAL files produced before devlog 082 will stamp `table_id = 0` (legacy/default).
6. Strict SQL fallback — was already implemented in `QueryExecutor::find_partitions` (Stage A); confirmed and covered by existing `TableScopedPartitioning.EmptyTableReturnsZeroRows` test.
7. `ClusterNode::ingest_tick` + query coordinator routing — remain symbol-id keyed on the hash ring; `table_id` travels inside the TickMessage payload and is preserved by `remote_ingest` (binary wire). No additional wiring needed beyond Stage A.

## Files Changed

| File | Change |
|---|---|
| `src/transpiler/python_binding.cpp` | `ingest_batch` / `ingest_float_batch` gain `const std::string& table_name = ""` kwarg; new private `resolve_table_id_()` helper throws `std::invalid_argument("Unknown table: ...")` on unknown name; pybind11 `.def(..., py::arg("table_name") = "")` |
| `zepto_py/dataframe.py` | `from_pandas / from_polars / from_polars_arrow / from_arrow` gain optional `table_name` kwarg; passes through to `pipeline.ingest_batch` only when non-empty (preserves compatibility with mock pipelines) |
| `zepto_py/arrow.py` | `ArrowSession.ingest_arrow` / `ingest_arrow_columnar` gain `table_name` kwarg |
| `include/zeptodb/feeds/kafka_consumer.h` | New `KafkaConfig::table_name`; new private `uint16_t table_id_` cache |
| `src/feeds/kafka_consumer.cpp` | `set_pipeline()` resolves `config_.table_name` via `SchemaRegistry`; unknown name logs ERROR and leaves `table_id_ = 0`; `ingest_decoded()` stamps `msg.table_id = table_id_` and drops with `ingest_failures++` if name was configured but unknown |
| `include/zeptodb/feeds/mqtt_consumer.h` | Same pattern — `MqttConfig::table_name`, private `table_id_` |
| `src/feeds/mqtt_consumer.cpp` | Same pattern in `set_pipeline()` / `ingest_decoded()` |
| `include/zeptodb/feeds/fix_parser.h` | `FIXParser` gains `set_table_id / set_table_name / table_id() / table_name()` setters (write-only hints; upstream code stamps on outgoing TickMessage) |
| `include/zeptodb/feeds/nasdaq_itch.h` | Same setters on `NASDAQITCHParser` |
| `include/zeptodb/feeds/binance_feed.h` | Same setters on `BinanceFeedHandler` |
| `include/zeptodb/migration/clickhouse_migrator.h` | `ClickHouseMigrator::Config::dest_table` (std::string, default empty) |
| `include/zeptodb/migration/duckdb_interop.h` | `DuckDBIntegrator::Config::dest_table` |
| `include/zeptodb/migration/timescaledb_migrator.h` | `TimescaleDBMigrator::Config::dest_table` |
| `include/zeptodb/migration/hdb_loader.h` | `HDBLoader::set_dest_table / dest_table()` accessors |
| `tools/zepto-migrate.cpp` | New `--dest-table <name>` CLI flag wired into all four migrator configs |
| `tests/unit/test_kafka.cpp` | **+3 tests**: `TableScopedIngestLandsInTable`, `TableNameEmptyIsLegacyPath`, `UnknownTableDropsMessages` |
| `tests/unit/test_mqtt.cpp` | **+2 tests**: `TableScopedIngestLandsInTable`, `UnknownTableDropsMessages` |
| `tests/feeds/test_fix_parser.cpp` | **+1 test**: `FIXParserStageB.TableIdSetterRoundTrip` |
| `tests/feeds/test_nasdaq_itch.cpp` | **+1 test**: `NASDAQITCHParserStageB.TableIdSetterRoundTrip` |
| `tests/migration/test_hdb_loader.cpp` | **+4 tests**: `DestTableDefaultsEmpty`, `DestTableSetterRoundTrip`, `MigrationConfig.DestTableFieldsDefaultEmpty`, `MigrationConfig.DestTableFieldsAssignable` |
| `tests/python/test_table_aware_ingest.py` | **+4 tests**: `test_ingest_batch_table_name_lands_in_table`, `test_ingest_batch_unknown_table_raises`, `test_ingest_batch_empty_table_name_is_legacy`, `test_ingest_float_batch_table_name_unknown_raises` |

## Tests

```
$ ./tests/zepto_tests --gtest_filter="*Kafka*:*MQTT*:*Mqtt*:*Migration*:*TableScoped*:*Schema*:*HDB*"
[  PASSED  ] 94 tests.

$ ./tests/test_feeds
[  PASSED  ] 23 tests.  (was 21; +2 Stage B)

$ ./tests/test_migration
[  PASSED  ] 130 tests. (was 126; +4 Stage B)

$ ./tests/zepto_tests
[  PASSED  ] 1201 tests.   (was 1196 after Stage A; +5 new Stage B)

$ python3 -m pytest tests/python/
======================== 212 passed, 1 warning in 4.04s ========================
```

All four binaries — `zepto_tests`, `zepto_http_server`, `zepto_data_node`, `zepto-cli` — link cleanly. (`zepto-migrate` has a pre-existing link issue with `libzepto_migration.a` missing `libzepto_auth.a` that is unrelated to this devlog.)

## Backward Compatibility

- **Default kwargs are all empty / None.** Any caller not aware of table names keeps working unchanged (`msg.table_id = 0` → legacy path).
- **DataFrame helpers only forward `table_name` to C++ when it is truthy.** Mock pipelines in existing tests continue to accept the 3-arg `ingest_batch(symbols, prices, volumes)` signature without modification.
- **Feed handlers default to `table_name = ""`.** Pre-Stage-B configs are unaffected.
- **WAL format unchanged.** Raw 64-byte TickMessage serialization in `WALWriter::write` already carries `table_id` since devlog 082. WAL files written before that date replay with `table_id = 0` (legacy path).
- **ClusterNode routing unchanged.** Partition routing is symbol-id keyed; `table_id` is payload data that travels through `TcpRpcClient::ingest_tick` binary wire.

## Known Follow-ups (for Stages C–D)

- Feed handlers that emit the intermediate `Tick` struct (FIX / ITCH / Binance) expose only setters; full stamp-through to `TickMessage` requires call-site integration in whichever glue layer forwards `Tick → TickMessage` (typically in user code or a planned `feed_to_pipeline` adapter).
- Migration tools currently populate `dest_table` in their configs but do not yet feed rows through a live `pipeline.ingest_tick()` path; the field is wired so that when the live-pipeline ingest path lands, `msg.table_id` can be stamped without further API changes.
- Deeper distributed scenarios (cross-table query fan-out coordinator, cross-cluster catalog sync) are Stage D.

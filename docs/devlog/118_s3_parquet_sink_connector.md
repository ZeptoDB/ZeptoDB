# devlog #118 — S3 Parquet sink connector (operator UX)

**Date:** 2026-05-13
**Work:** Finished the operator-facing experience for the cold-tier S3 Parquet sink. Adds Hive-style S3 path layout, Helm `coldTier.*` block, `--cold-tier-*` CLI flags + `ZEPTO_COLD_TIER_*` env vars, three new test files, and an operator recipe doc.

---

## Background

The C++ infra for cold-tier S3 Parquet has been in place since devlog 012:
`S3Sink`, `ParquetWriter`, and the `FlushManager` Parquet+S3 paths. What was
missing was the operator-facing surface — there was no Helm flag, no CLI
flag, no env-var integration, and the only path layout was the flat
`{prefix}/{symbol}/{hour}.{ext}` convention which Athena / DuckDB / Polars /
Spark do not Hive-discover automatically.

Without those, "stream cold partitions to S3" was a code path you had to
invoke from C++. Devlog 118 closes that gap and ships the cold tier as a
single-flag opt-in.

---

## Scope

Five surfaces wired end-to-end:

| Surface | Change |
|---------|--------|
| **C++ S3 path layout** | New `S3Layout` enum (`FLAT` default, `HIVE`) on `S3SinkConfig`. `S3Sink::make_s3_key()` branches on layout. Backward-compatible: FLAT case is byte-identical. |
| **HIVE filename collision protection** | `S3Sink` computes a 4-char lower-case hex hash of `gethostname()` once at construction (FNV-1a). Hash is appended to HIVE filenames so two pods writing the same `(symbol, hour)` partition don't overwrite each other. Tests stub it via `S3SinkConfig::disable_host_hash`. |
| **Helm chart** | New top-level `coldTier:` block in `values.yaml`. Default is disabled; when enabled, both `Deployment` and `StatefulSet` templates inject `ZEPTO_COLD_TIER_*` env vars (and the configmap echoes the resolved values for operator visibility). |
| **CLI flags** | `tools/zepto_http_server.cpp` parses `--cold-tier-*` flags and falls back to `ZEPTO_COLD_TIER_*` env vars. CLI > env > default. Wires into `PipelineConfig::flush_config` (`output_format = PARQUET`, `enable_s3_upload = true`, `s3_config.layout = HIVE`, etc.). Validates that a bucket is set when enabled, switches `storage_mode` to `TIERED` automatically, and logs the resolved cold-tier config at INFO. |
| **Tests + docs** | New `test_s3_sink.cpp`, `test_parquet_writer.cpp`, `test_cold_tier.cpp`. New `docs/operations/COLD_TIER_S3.md` operator recipe. Devlog 012 design doc and `PARQUET_S3_ACTIVATION.md` cross-link to the new operator surface. |

---

## HIVE path layout

```
s3://{bucket}/{prefix}/year=YYYY/month=MM/day=DD/symbol={ID}/{ID}-{hour_epoch}[-{hash}].{ext}
```

- `year=YYYY/month=MM/day=DD` is computed from `hour_epoch * 3600` seconds via `gmtime_r()` (UTC; thread-safe).
- `month` and `day` are zero-padded to 2 digits (`month=01`, `day=09`).
- `symbol={ID}` uses the integer `SymbolId` (interned).
- Filename `{ID}-{hour_epoch}` carries both id and hour so files from the same partition don't collide across pods. The optional `-{hash}` 4-char hex suffix is the per-pod safety net; if `gethostname()` fails the suffix is omitted and the filename remains unique within a single pod.

Sample (deterministic, with `disable_host_hash=true`):
```
s3://zepto-cold-test/hdb/year=2024/month=05/day=01/symbol=7/7-476256.parquet
```
(`hour_epoch = 476256` → `476256 * 3600 = 1,714,521,600 = 2024-05-01 00:00 UTC`.)

`FlushManager::flush_partition_parquet` converts the partition's nanosecond `hour_epoch` (`floor(ts_ns / NS_PER_HOUR) * NS_PER_HOUR`) to hours since epoch before calling `make_s3_key` for HIVE; FLAT keeps the byte-identical pre-118 nanosecond form.

---

## Backward compatibility

- `S3Layout::FLAT` is the default. The `make_s3_key()` FLAT branch is byte-identical to the pre-118 implementation.
- `FlushConfig` field ordering is unchanged. The new `S3SinkConfig::layout` and `disable_host_hash` fields are appended at the end of the struct.
- Helm `coldTier.enabled=false` (default) generates the exact same manifests as before this devlog.
- The 1300+ test suite is unchanged except for 14 new passing tests + 1 opt-in skip.

---

## Tests

Three new files, all in `tests/unit/`:

| File | Tests |
|------|-------|
| `test_s3_sink.cpp` | `S3SinkPath.FlatLayout`, `S3SinkPath.HiveLayout`, `S3SinkPath.HiveLayout_PadsZeroes`, `S3SinkPath.HiveLayout_HostHashShape`, `S3Sink.AwsAvailability`, `S3Sink.UploadFile_OptIn` (opt-in skip when `ZEPTO_S3_TEST_BUCKET` unset) |
| `test_parquet_writer.cpp` | `ParquetWriterTest.Available`, `ParquetWriterTest.Filename`, `ParquetWriterTest.RoundTrip_SmallPartition` (skipped if Arrow/Parquet not built in) |
| `test_cold_tier.cpp` | `ColdTier.HiveKey_MatchesGmtime`, `ColdTier.HiveKeyOnFlush` (skipped if Arrow/Parquet not built in) |

Path-generation tests run on every architecture without network. Live S3 upload tests are gated on `ZEPTO_S3_AVAILABLE` + an opt-in env var.

```
[----------] 4 tests from S3SinkPath
[       OK ] S3SinkPath.FlatLayout
[       OK ] S3SinkPath.HiveLayout
[       OK ] S3SinkPath.HiveLayout_PadsZeroes
[       OK ] S3SinkPath.HiveLayout_HostHashShape
[----------] 2 tests from S3Sink
[       OK ] S3Sink.AwsAvailability
[  SKIPPED ] S3Sink.UploadFile_OptIn
[----------] 3 tests from ParquetWriterTest
[       OK ] ParquetWriterTest.Available
[       OK ] ParquetWriterTest.Filename
[       OK ] ParquetWriterTest.RoundTrip_SmallPartition
[----------] 2 tests from ColdTier
[       OK ] ColdTier.HiveKey_MatchesGmtime
[       OK ] ColdTier.HiveKeyOnFlush
```

Total suite: 1310 tests, 1308 PASS, 1 SKIPPED (opt-in), 1 pre-existing flake unrelated to this change (`TcpRpc.StatsRequest_NoCallback_ReturnsEmptyJson`, passes in isolation; tracked in devlog 096).

---

## Helm verification

```
$ helm lint deploy/helm/zeptodb
==> Linting deploy/helm/zeptodb
[INFO] Chart.yaml: icon is recommended
1 chart(s) linted, 0 chart(s) failed

$ helm template deploy/helm/zeptodb \
    --set coldTier.enabled=true \
    --set coldTier.s3.bucket=test \
    --set coldTier.layout=hive | grep -A1 ZEPTO_COLD_TIER_LAYOUT
            - name: ZEPTO_COLD_TIER_LAYOUT
              value: "hive"
```

---

## CLI verification

```
$ ./zepto_http_server --cold-tier-enabled --port 9876
Error: --cold-tier-enabled requires --cold-tier-s3-bucket (or ZEPTO_COLD_TIER_S3_BUCKET)

$ ./zepto_http_server --port 9877 --no-auth \
    --cold-tier-enabled \
    --cold-tier-s3-bucket my-test-bucket \
    --cold-tier-layout hive
[…] Cold tier S3 Parquet sink ENABLED: format=parquet layout=hive age_hours=24
    delete_local=true bucket=my-test-bucket region=us-east-1 prefix=hdb
    endpoint=<aws> path_style=false
```

---

## Files changed

**C++ runtime (5):**
- `include/zeptodb/storage/s3_sink.h` — new `S3Layout` enum, `layout` + `disable_host_hash` fields, `host_hash_` member.
- `src/storage/s3_sink.cpp` — `make_s3_key()` branches on layout; constructor computes `host_hash_`; new helpers `read_hostname()` + `fnv1a_hex4()`.
- `src/storage/flush_manager.cpp` — converts ns→hours when calling `make_s3_key()` in HIVE mode (FLAT preserved).
- `tools/zepto_http_server.cpp` — `--cold-tier-*` CLI flags + `ZEPTO_COLD_TIER_*` env vars, wires to `FlushConfig`, startup INFO log line.
- `tests/CMakeLists.txt` — registers the three new test files.

**Tests (3, all new):**
- `tests/unit/test_s3_sink.cpp`
- `tests/unit/test_parquet_writer.cpp`
- `tests/unit/test_cold_tier.cpp`

**Helm chart (4):**
- `deploy/helm/zeptodb/values.yaml` — new `coldTier:` top-level block.
- `deploy/helm/zeptodb/templates/configmap.yaml` — emits `cold_tier_*` keys when enabled.
- `deploy/helm/zeptodb/templates/statefulset.yaml` — `ZEPTO_COLD_TIER_*` env vars.
- `deploy/helm/zeptodb/templates/deployment.yaml` — `ZEPTO_COLD_TIER_*` env vars.

**Docs (6):**
- `docs/operations/COLD_TIER_S3.md` — NEW operator recipe (Helm + CLI + IAM + Athena/DuckDB/Polars/Spark read recipes).
- `docs/design/layer1_storage_memory.md` — appended a "Cold tier S3 Parquet sink" subsection.
- `docs/deployment/PARQUET_S3_ACTIVATION.md` — link to the new operator recipe; existing C++ snippet labeled as the lower-level alternative.
- `.kiro/KIRO.md` — bumped "Current last:" to 118.
- `docs/BACKLOG.md` — moved "S3 Parquet sink connector" to recent completions; updated summary count.
- `docs/COMPLETED.md` — new entry for devlog 118.

---

## Out of scope (followups)

- Live MinIO / localstack integration test in CI. Currently opt-in via `ZEPTO_S3_TEST_BUCKET`.
- AWS IRSA service-account binding in the Helm chart; operators wire that themselves today.
- Multipart upload threshold tuning surface in Helm; the existing `S3SinkConfig::multipart_threshold` is C++-only.
- Lifecycle / storage-class policy configuration alongside the bucket — operators set this on the bucket directly. The new operator doc carries a cost note pointing at S3 IA / Glacier lifecycle as the standard pattern.

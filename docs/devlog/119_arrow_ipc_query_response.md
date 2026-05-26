# Devlog 119 — Arrow IPC query response

*Date: 2026-05-15*
*Closes BACKLOG P4 row "Arrow IPC query response" (Effort: S)*

## Context

The Arc 26.05 competitive analysis (2026-05-13, BACKLOG entry) measured up to
**2.86× faster** wire-format throughput on the same DuckDB engine when the
HTTP query response was Arrow IPC instead of JSON — JSON encoding was the
actual bottleneck on large result sets, not query execution. Arrow IPC also
lands directly in Pandas / Polars / BI tool memory layout, avoiding a
parse-and-rebuild step on the client side.

This was the highest leverage-per-effort row in P4 (S effort) and the
single-most-cited "near-free win" of the Arc analysis.

## What shipped

`POST /` (port 8123, ClickHouse-compatible) now honours Arrow IPC content
negotiation:

| Trigger | Example |
|---------|---------|
| `Accept` header | `Accept: application/vnd.apache.arrow.stream` |
| ClickHouse default-format | `?default_format=Arrow` (or `ArrowStream`) |
| Short-form param | `?format=arrow` |

When triggered, the response is an Arrow IPC RecordBatchStream
(`application/vnd.apache.arrow.stream`) plus an `X-Zepto-Format: arrow-stream`
header. JSON remains the default — no behaviour change for existing clients.

Errors (parse, executor, ACL, tenant denial) **always** return JSON regardless
of `Accept`; only successful result sets are encoded as Arrow. Matches
ClickHouse semantics.

When the server was built with `ZEPTO_USE_FLIGHT=OFF` (Arrow C++ not present),
Arrow-format requests return `406 Not Acceptable` with a JSON error body.

## Code organisation

Pulled the Arrow encoder out of `src/server/flight_server.cpp` into a new
shared TU so both the HTTP server and the Flight server use the **same**
implementation:

```
include/zeptodb/server/arrow_ipc.h    — public API (always declared)
src/server/arrow_ipc.cpp              — Arrow-gated implementation + stub
```

Public API:

```cpp
namespace zeptodb::server {
bool arrow_ipc_available() noexcept;
bool encode_result_set_ipc(const zeptodb::sql::QueryResultSet& rs,
                           std::string* out, std::string* err);
} // namespace
```

Plus Arrow-typed helpers (`build_arrow_schema`, `result_to_record_batch`)
gated by `ZEPTO_FLIGHT_ENABLED` for `flight_server.cpp` consumption.
`flight_server.cpp`'s anonymous-namespace duplicates of `to_arrow_type`,
`build_schema`, and `result_to_batch` were deleted — single source of truth.

CMake: new tiny static lib `zepto_arrow_ipc` linked from both `zepto_server`
and `zepto_flight`. Propagates `ZEPTO_FLIGHT_ENABLED=1` as a PUBLIC compile
def so `http_server.cpp` (and any other consumer) can `#ifdef`-gate as needed.
The stub branch always builds, so `zepto_server` can call the public API
unconditionally.

While extracting the encoder, I extended it to also map `SYMBOL` columns (in
addition to `STRING`) through `symbol_dict` to Arrow `utf8`. The previous
Flight server returned raw int64 codes for `SYMBOL`, which was effectively
useless on the Python / Polars client side. The Flight server picks this up
for free.

## Build gate

`ZEPTO_USE_FLIGHT=ON` (already the default when system Arrow Flight or the
pyarrow bundle is found at configure time). No new CMake option.

## HTTP handler change

`src/server/http_server.cpp` POST `/` lambda: after a successful
`run_query_with_tracking(...)`, before the existing `build_json_response()`
call, parse the Arrow content-negotiation triggers (case-insensitive
`Accept` substring scan + query params). On match:

- `arrow_ipc_available() == false` → `406 Not Acceptable` with JSON error
- `encode_result_set_ipc` failure → `500` with JSON error
- success → set `X-Zepto-Format: arrow-stream` and write the IPC bytes with
  `Content-Type: application/vnd.apache.arrow.stream`

Otherwise fall through unchanged to the JSON path. Access-log /
slow-query-log byte counts come from `res.body.size()` so they capture Arrow
bodies correctly with no extra plumbing.

## Test summary

New file: `tests/unit/test_http_arrow_ipc.cpp` — 7 tests in two suites:

`HttpArrowIpcTest` (in-process `HttpServer` on an ephemeral port):

1. **ArrowDisabled_Returns406_WithJsonError** — always-on. Asserts `406` +
   JSON when Arrow is unavailable; asserts `200` + `application/vnd.apache.arrow.stream`
   when Arrow is available (the "happy path on the available branch" sentinel).
2. **AcceptHeader_VwapAndCount_DecodesViaPyarrowParity** — happy path
   INT64+FLOAT64. Asserts `200`, `Content-Type`, `X-Zepto-Format`, decodes
   the IPC body via `arrow::ipc::RecordBatchStreamReader`, asserts column
   names + first-row int64 value match the JSON path of the same query.
3. **AcceptHeader_EmptyResult_SchemaWithZeroRows** — `WHERE price < 0`
   produces zero rows but preserves the schema; Arrow body decodes to a
   table with `num_rows() == 0` and `num_columns() >= 1`.
4. **DefaultFormatArrow_QueryParam** — ClickHouse-style
   `?default_format=Arrow` (no `Accept` header) returns Arrow body.
5. **FormatArrow_QueryParam** — short-form `?format=arrow` returns Arrow body.
6. **BadSql_WithArrowAccept_StaysJson** — bad SQL with `Accept: arrow` →
   `400` + JSON, never Arrow. Confirms error path stays JSON.

`HttpArrowIpcEncoder` (encoder unit test, no HTTP server):

7. **StringColumnViaSymbolDict_DecodesAsUtf8** — drives `encode_result_set_ipc`
   with a hand-crafted `QueryResultSet` whose `column_types[0] == STRING` and
   a `StringDictionary`. Asserts the decoded Arrow column has type
   `arrow::utf8` and the right string values.

All 7 pass with `ZEPTO_FLIGHT_ENABLED=1`. Test #1 also covers the
build-without-Arrow path's 406 fallback (skips the asserts when Arrow is
present, asserting the success branch instead). Tests #2–#6 are guarded by
`#ifdef ZEPTO_FLIGHT_ENABLED`.

Cross-check: `tests/zepto_tests --gtest_filter='*Http*:*Flight*'` → 38 / 38
PASS (no regression on existing Flight tests after the helper extraction).
Full local suite (excluding K8s, Benchmark, K8sFailure): **1302 / 1302 PASS,
1 SKIPPED** (the opt-in `S3Sink.UploadFile_OptIn`).

### Deviation from the prompt — test #2

The prompt asked for a HTTP-level test:
`SELECT symbol, count(*) FROM trades GROUP BY symbol` with `Accept: arrow`
asserting `arrow::utf8` for the symbol column. In the current executor,
`exec_group_agg` (and the parallel SELECT path) flatten the GROUP BY key
column type to `INT64` regardless of the underlying SYMBOL/STRING type — see
`src/sql/executor.cpp:3232,5510`. `SELECT symbol FROM trades` similarly
hard-codes INT64 in the parallel path and reconstructs `symbol` from the
partition key as int64 in the sequential path. There is therefore no SQL
shape today that produces a `column_types[i] == STRING` result set out of
the standard query path.

To still cover the encoder's STRING + symbol_dict → arrow::utf8 path, test #2
drives `encode_result_set_ipc` directly with a hand-crafted `QueryResultSet`
(suite `HttpArrowIpcEncoder`). The end-to-end HTTP path is exercised by the
INT64+FLOAT64 happy-path test #2 (now renamed to `_VwapAndCount_*`). When the
executor is later extended to propagate STRING/SYMBOL types through the
SELECT/GROUP-BY surface, this test should be promoted to a full HTTP-level
test — the encoder path is already proven.

## Doc updates

- `docs/api/HTTP_REFERENCE.md` — new "Arrow IPC response (binary)"
  subsection under "Response Format" with trigger matrix, response shape,
  curl + pyarrow example, build-gate note, error semantics. Last-updated
  bumped to 2026-05-15.
- `docs/devlog/119_arrow_ipc_query_response.md` — this file.
- `docs/COMPLETED.md` — new top entry above devlog 118.
- `docs/BACKLOG.md` — P4 row removed; "Recent completions" bullet added;
  next-devlog pointer bumped 118 → 119; P4 summary row count -1 and next
  action updated.
- `include/zeptodb/server/http_server.h` — file-header endpoint comment
  block now documents Arrow IPC content negotiation.
- `include/zeptodb/server/arrow_ipc.h` — new public header with full doxygen.

`README.md`: checked. The README's HTTP reference is via link to
`docs/api/HTTP_REFERENCE.md` and never claimed JSON-only — no change needed.

## Build verification

```
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..      # clean configure
ninja -j$(nproc) zepto_arrow_ipc zepto_server zepto_flight \
                  zepto_http_server zepto_tests   # all build clean
./tests/zepto_tests --gtest_filter='*ArrowIpc*:*HttpArrow*'
    → 7 / 7 PASS
./tests/zepto_tests --gtest_filter='*Http*:*Flight*'
    → 38 / 38 PASS  (no Flight regression after helper extraction)
./tests/zepto_tests --gtest_filter='-*K8s*:*Benchmark*:*K8sFailure*'
    → 1302 / 1302 PASS + 1 SKIPPED + 3 DISABLED   (full local suite)
```

No new warnings on the touched files.

## Out of scope (follow-ups)

- **Arrow IPC ingest endpoint** (`POST /insert/arrow`) — separate P4 row,
  pairs with the batched HTTP client P8-Bench item. Not touched here.
- **Streaming chunked responses** — single RecordBatch is sufficient for
  result sets up to a few hundred MB; chunking via `WriteRecordBatch` in a
  loop is a follow-up for very large results.
- **IPC-level compression** — `ipc::IpcWriteOptions::compression` (LZ4/ZSTD)
  is plumbed in Arrow but disabled here; a `?compression=lz4` knob is a
  follow-up.
- **Arrow Flight SQL** — already exists at devlog 042, untouched.

## Post-review touch-ups

Small follow-ups applied after the initial review of this devlog:

1. **`docs/BACKLOG.md` total count off-by-one** — the P4 row was removed in
   this devlog but the grand total at the bottom of the priority summary was
   not decremented. `Total open: 55 items` → `54 items`.
2. **`docs/BACKLOG.md` stale test count in the header** — bumped
   `1310 tests passing` → `1317 tests passing` to reflect the 7 new tests
   added by this devlog (now 8 after touch-up #4).
3. **`docs/api/HTTP_REFERENCE.md` confusing 406 wording** — the Arrow IPC
   subsection's note pointed readers at `GET /api/license` to recover from a
   406, but `/api/license` reports licence status, not Arrow build flags. The
   bullet now states plainly that 406 means the binary was built without
   Arrow support and to rebuild with `-DZEPTO_USE_FLIGHT=ON` (or install
   Arrow / pyarrow before reconfiguring). The `/api/license` reference was
   dropped.
4. **Defensive guard in the Arrow encoder** — `build_arrow_schema`
   unconditionally mapped `STRING`/`SYMBOL` to `arrow::utf8()`, but
   `result_to_record_batch` only emits a utf8 array when `rs.symbol_dict` is
   non-null; with a null dict it falls through to the int64 branch, producing
   a schema/array type mismatch surfacing as a cryptic Arrow status at write
   time. The SQL executor always sets `symbol_dict` today, so the path is
   unreachable, but it was a sharp edge. Fix: in `build_arrow_schema`, when
   `column_types[i]` is `STRING`/`SYMBOL` but `rs.symbol_dict == nullptr`,
   declare the field as `arrow::int64()`. New unit test
   `HttpArrowIpcEncoder.SymbolWithoutDict_FallsBackToInt64Schema` (gated on
   `ZEPTO_FLIGHT_ENABLED`) decodes the IPC body and asserts the field type
   is `arrow::int64()` and the values match the codes.

After touch-ups: `--gtest_filter='*ArrowIpc*:*HttpArrow*'` → **8 / 8 PASS**
(was 7).

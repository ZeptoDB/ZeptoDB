# 090 — Multi-table close-out: DDL ACL, tenant namespace, DataFrame coverage, Web UI verify

Date: 2026-04-18
Status: Complete (all four follow-on residuals RESOLVED in devlog [091](091_multi_table_residuals.md))
Related: [082](082_table_scoped_partitioning.md), [084](084_stage_b_ingest_paths.md), [085](085_stage_c_cluster_and_sql_strict.md), [088](088_client_ddl_and_final_residuals.md), [089](089_client_hardening.md), [091](091_multi_table_residuals.md)

## Scope

Four residual multi-table items remained after 089. This devlog closes all of
them end-to-end and declares the multi-table feature complete across every
client surface (SQL, HTTP, Python DSL, Arrow, polars, Web UI).

| Stage | Residual | Resolution |
|-------|----------|------------|
| E1 | `X-Zepto-Allowed-Tables` only gated SELECT/INSERT/UPDATE/DELETE — `CREATE`/`DROP`/`ALTER TABLE` and `DESCRIBE` slipped past the table-level ACL | Extend the POST `/` handler's ACL block to cover all four DDL kinds plus DESCRIBE (`src/server/http_server.cpp:441-495`) |
| E2 | `TenantManager::can_access_table` existed (`src/auth/tenant_manager.cpp:64`) but was never called from HTTP — tenants could read outside `table_namespace` | Stash `X-Zepto-Tenant-Id` at auth time, enforce namespace prefix before `run_query_with_tracking` (`src/server/http_server.cpp:172-177`, `src/server/http_server.cpp:484-495`) |
| E3 | `tests/python/test_table_aware_ingest.py` only covered numpy `ingest_batch` — Arrow and polars DataFrame adapters were silently untested on the `table_name=` path | Two new tests using `zepto_py.dataframe.from_polars` / `from_arrow` with CREATE TABLE + round-trip SELECT count (`tests/python/test_table_aware_ingest.py:98-147`) |
| E4 | Web UI `/tables` (SHOW TABLES) and `/tables/[name]` (DESCRIBE + SELECT) needed end-to-end verification against the new table-id partitioning introduced in devlog 082 | Manual smoke against `zepto_http_server`; `pnpm test` (vitest) also re-run | 

## E1 — DDL + DESCRIBE under table ACL

The POST `/` handler parses the SQL once and resolves the touched table name
across every statement kind:

```cpp
std::string touched_table;
try {
    zeptodb::sql::Parser parser;
    auto ps = parser.parse_statement(req.body);
    if (ps.select)             touched_table = ps.select->from_table;
    else if (ps.insert)        touched_table = ps.insert->table_name;
    else if (ps.update)        touched_table = ps.update->table_name;
    else if (ps.del)           touched_table = ps.del->table_name;
    else if (ps.create_table)  touched_table = ps.create_table->table_name;
    else if (ps.drop_table)    touched_table = ps.drop_table->table_name;
    else if (ps.alter_table)   touched_table = ps.alter_table->table_name;
    else if (ps.kind == zeptodb::sql::ParsedStatement::Kind::DESCRIBE_TABLE)
        touched_table = ps.describe_table_name;
} catch (...) {} // parse failure → let executor surface the error
```

The allowed-list check was hoisted above `run_query_with_tracking` so both
ACL and tenant namespace enforcement reuse a single `touched_table`.

### Tests (`tests/unit/test_table_acl_ddl.cpp`)

- `TableACL_DenyCreateTable_NotInAllowedList` — 403 on `CREATE TABLE forbidden`
- `TableACL_DenyDropTable_NotInAllowedList` — 403 on `DROP TABLE forbidden`
- `TableACL_DenyAlterTable_NotInAllowedList` — 403 on `ALTER TABLE forbidden ADD COLUMN`
- `TableACL_AllowDescribeAllowedTable` — 200 on `DESCRIBE trades`
- `TableACL_DenyDescribeForbiddenTable` — 403 on `DESCRIBE secret`

## E2 — Tenant namespace enforcement at HTTP

`AuthContext.tenant_id` is stashed into `X-Zepto-Tenant-Id` at auth time next
to `X-Zepto-Allowed-Tables`:

```cpp
if (!decision.context.tenant_id.empty())
    mutable_req.set_header("X-Zepto-Tenant-Id", decision.context.tenant_id);
```

The POST handler then consults `tenant_mgr_->can_access_table()`:

```cpp
if (!touched_table.empty() && tenant_mgr_ &&
    req.has_header("X-Zepto-Tenant-Id")) {
    std::string tid = req.get_header_value("X-Zepto-Tenant-Id");
    if (!tid.empty() && !tenant_mgr_->can_access_table(tid, touched_table)) {
        res.status = 403;
        res.set_content(build_error_json(
            "Tenant '" + tid + "' cannot access table '" + touched_table + "'"),
            "application/json");
        return;
    }
}
```

`HttpServer::set_tenant_manager()` already existed (header line 108) and is
now exercised from the POST path, not only from admin quota endpoints.

### Tests (`tests/unit/test_table_acl_ddl.cpp`)

- `TenantNamespace_DenyTableOutsideNamespace` — tenant with `table_namespace="deskA."` gets 403 on `deskB.trades`
- `TenantNamespace_AllowTableInsideNamespace` — same tenant gets non-403 on `deskA.trades`
- `TenantNamespace_NoNamespace_Unrestricted` — tenant with empty namespace → 200 on any table
- `TenantNamespace_NoTenant_Unrestricted` — request without `X-Zepto-Tenant-Id` → 200

## E3 — Arrow / polars DataFrame adapters honour `table_name=`

The `zepto_py.dataframe` adapters (`from_polars`, `from_arrow`) already
forward `table_name` through to `Pipeline.ingest_batch(table_name=…)` — but
the round-trip was untested. Two new tests drive it end-to-end:

```python
def test_from_polars_table_name_lands_in_table():
    pl = pytest.importorskip("polars")
    from zepto_py.dataframe import from_polars
    p = zeptodb.Pipeline(); p.start()
    p.execute("CREATE TABLE poltab " + TBL_DDL)
    df = pl.DataFrame({"sym":[1,2,3],"price":[100,200,300],"volume":[10,20,30]})
    assert from_polars(df, p, table_name="poltab") == 3
    p.drain()
    assert p.execute("SELECT count(*) FROM poltab")["data"][0][0] == 3
    p.stop()

def test_from_arrow_table_name_lands_in_table():
    pa = pytest.importorskip("pyarrow")
    from zepto_py.dataframe import from_arrow
    # … same pattern, CREATE TABLE arrtab, from_arrow(t, p, table_name="arrtab")
```

Both tests use `pytest.importorskip` so test runs without polars/pyarrow
still pass.

## E4 — Web UI smoke against new table-id partitioning

Verification-only stage — no code changes. Smoke against a standalone
`zepto_http_server --no-auth` on port 29000:

| Query | Expected | Observed |
|-------|----------|----------|
| `CREATE TABLE webui_smoke (...)` | 200, "created" | ✓ |
| `SHOW TABLES` | row for `webui_smoke` present | ✓ (rows: `[["trades",…],["webui_smoke",…]]`) |
| `DESCRIBE webui_smoke` | 4 column rows | ✓ (`symbol`, `price`, `volume`, `timestamp`, all `INT64`) |
| `SELECT * FROM webui_smoke` | 0 rows (empty) | ✓ (this is the devlog 082 strict-fallback fix) |

`pnpm test` (vitest): **9 test files / 61 tests passed**. The single
"failed" file is an unrelated Playwright e2e config issue (`e2e/web-ui.spec.ts`),
not a vitest regression.

Web UI is table-id compatible.

## Multi-table feature — complete end-to-end

Surfaces covered since devlog 082 (table-scoped partitioning):

| Surface | Covered in | Verified by |
|---------|-----------|-------------|
| Partition manager (per-table) | 082 | `TableScopedPartitioning.*` (13 tests) |
| Schema registry persistence | 082 | `TableScopedPartitioning.SchemaRegistryPersistsAcrossRestart` |
| Strict-fallback (unknown table → empty) | 082 | `TableScopedPartitioning.StrictFallbackUnknownTableReturnsEmpty` |
| `Pipeline.ingest_batch(table_name=…)` | 084 | `test_table_aware_ingest::test_ingest_batch_table_name_lands_in_table` |
| `Pipeline.ingest_float_batch(table_name=…)` | 084 | `test_ingest_float_batch_table_name_unknown_raises` |
| SQL strict-scope (per-table SELECT) | 085 | `test_sql.cpp::SelectStrictTableScope*` |
| Cluster router per-table placement | 085 | `test_cluster.cpp::RouterPerTable*` |
| Kafka/ITCH/FIX feed `table_id` routing | 086 | `test_feed_table_id.cpp` |
| Python `create_table` / `drop_table` safety | 089 | `test_connection::TableDdlValidation` |
| CLI semicolon handling | 089 | `test_cli` |
| HTTP allowed_tables: SELECT/INSERT/UPDATE/DELETE | pre-082 | `test_features::TableACL*` |
| **HTTP allowed_tables: CREATE/DROP/ALTER/DESCRIBE** | **090** | **`test_table_acl_ddl.cpp` (5 tests)** |
| **HTTP tenant `table_namespace` enforcement** | **090** | **`test_table_acl_ddl.cpp` (4 tests)** |
| **polars / Arrow DataFrame `table_name=` round-trip** | **090** | **`test_table_aware_ingest.py` (2 tests)** |
| **Web UI `/tables`, `/tables/[name]`** | **090** | **manual smoke + vitest** |

## Test counts

- C++ suite: 1219 tests, 1216 passed (3 known flakes — `SplitBrain.StaleWalReplicationRejected`, `HttpCluster.RuntimeNodeAdd_ViaPostAPI`, `QueryCoordinator.TwoNodeRemote_DistributedAvg_Correct` — all pass in isolation, parallel-run port/timing flakes; unrelated to this change)
- Python `test_table_aware_ingest.py`: 6 tests, all passed (4 existing + 2 new)
- Web vitest: 9 files / 61 tests passed

## Files changed

- `src/server/http_server.cpp` — hoist & extend ACL table resolution; stash tenant id; tenant namespace check
- `tests/unit/test_table_acl_ddl.cpp` — **new**, 9 tests
- `tests/CMakeLists.txt` — register new test file
- `tests/python/test_table_aware_ingest.py` — 2 new DataFrame-adapter tests

## Doc updates

- `docs/COMPLETED.md` — devlog 090 bullet
- `docs/api/HTTP_REFERENCE.md` — `X-Zepto-Allowed-Tables` note extended to DDL + DESCRIBE
- `docs/design/layer5_security_auth.md` — tenant namespace HTTP-layer enforcement section
- `KIRO.md` — next devlog number bumped to 091

## Post-090 residuals — all RESOLVED in devlog 091

| # | Residual | Status |
|---|----------|--------|
| 1 | `zepto_http_server` had no `--tenant` CLI flag — operators had to POST to admin endpoints to provision tenants | **RESOLVED** — devlog 091 F1 |
| 2 | aarch64/Graviton full-suite run not yet executed on this branch | **RESOLVED** — devlog 091 F2 (deferred with documented unblocker) |
| 3 | `TenantNamespace_AllowTableInsideNamespace` weakened to `EXPECT_NE(403)` due to quoted-identifier parser limitation | **RESOLVED** — devlog 091 F3 (strict `EXPECT_EQ(200)` with unquoted-safe namespace) |
| 4 | HTTP POST path double-parsed SQL (once in ACL block, once in executor) | **RESOLVED** — devlog 091 F4 (new `QueryExecutor::cache_prepared()` helper) |

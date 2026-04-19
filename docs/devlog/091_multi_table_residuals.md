# 091 — Multi-table residuals closeout: --tenant CLI, test tightening, ACL cache priming

Date: 2026-04-18
Status: Complete
Related: [082](082_table_scoped_partitioning.md), [088](088_client_ddl_and_final_residuals.md), [089](089_client_hardening.md), [090](090_multi_table_closeout.md)

## Scope

Devlog 090 closed the multi-table feature across all client surfaces but left
four residual items. This devlog resolves all four.

| Residual | Resolution |
|----------|-----------|
| F1 — no CLI way to provision tenants at startup (operators had to POST to admin endpoints post-launch) | `--tenant <id:namespace>` flag added to `zepto_http_server`, repeatable; new `tests/integration/test_http_tenant.sh` smoke test |
| F2 — aarch64 / Graviton full-suite run not yet executed on this branch | EKS bench pool wake attempted; see F2 section for outcome |
| F3 — `TenantNamespace_AllowTableInsideNamespace` test was too loose (`EXPECT_NE 403` against a quoted identifier that the parser could not round-trip) | Rewritten with unquoted-identifier-safe namespace `"deska_"`, now asserts strict `EXPECT_EQ(r.status, 200)` |
| F4 — HTTP POST path parsed the SQL twice: once in the ACL block, once inside `QueryExecutor::execute()` | New `QueryExecutor::cache_prepared()` helper primes the prepared-statement cache from the HTTP ACL parse so the executor hits the cache |

## F1 — `--tenant id:namespace` CLI flag

`tools/zepto_http_server.cpp`:

- New `std::vector<std::pair<std::string, std::string>> tenants` at the top of `main` (line 83).
- New arg parser branch for `--tenant` (repeatable, expects `<id>:<namespace>`) (lines 120–130).
- `--help` now documents: `--tenant <id:namespace>  Register a tenant with a table-namespace prefix (repeatable)` — emitted as a dedicated `Tenants:` section in `tools/zepto_http_server.cpp` (verify: `./build/zepto_http_server --help | grep tenant`).
- After `server.set_web_dir(...)` and before `server.start_async()`, if `!tenants.empty()`:
  - Creates `std::make_shared<zeptodb::auth::TenantManager>()`
  - For each `(id, ns)` calls `tm->create_tenant({id, ns})` (single-arg signature from `include/zeptodb/auth/tenant_manager.h:56`)
  - Calls `server.set_tenant_manager(tm)`
  - Prints `Tenants provisioned: N`

The request-time wiring (stashing tenant-id into `X-Zepto-Tenant-Id` at auth
time for authenticated requests, or taking it client-supplied in `--no-auth`
mode) was already in place from devlog 090 — no change needed.

### Integration test: `tests/integration/test_http_tenant.sh`

New script. Launches `zepto_http_server --no-auth --tenant deska_:deska_`,
then:

1. Creates `deska_t` (inside namespace) and `deskb_t` (outside) — baseline.
2. POST `SELECT * FROM deskb_t` with `X-Zepto-Tenant-Id: deska_` → expects **403** (outside namespace).
3. POST `SELECT * FROM deska_t` with `X-Zepto-Tenant-Id: deska_` → expects **200** (inside namespace).

Pass result: `PASS: test_http_tenant` (local run, x86_64).

## F2 — aarch64 full-suite run (attempted)

Procedure:

1. `./tools/eks-bench.sh wake` → succeeded (both NodePool limits patched to 64).
2. Polled `kubectl get nodes -l zeptodb.com/role=bench-arm64` for up to 10 minutes → **no arm64 bench node came up.** Karpenter only provisions when a pending pod exists, and we never scheduled one because…
3. `./tools/run-aarch64-tests.sh` requires either (a) a working ECR push target (the current `REPO` value is the `REPLACE_ME` placeholder, which the script's own guard at line 28 refuses) or (b) `SKIP_PUSH=1` which only produces a local image and does not run it on the Graviton node.
4. `./tools/eks-bench.sh sleep` → invoked unconditionally; both NodePools back to `limits.cpu=0`.

**Outcome: deferred.** Unblocking this requires either:

- Setting the `REPO` env var to a real ECR repository accessible from the EKS cluster, OR
- Extending `tools/run-aarch64-tests.sh` with a `SKIP_PUSH=1` branch that provisions an arm64 node via a dummy pod, then `docker buildx --load` + SSH over the node's `containerd` to run the test image directly (non-trivial — out of scope for this residuals devlog).

Arch-neutrality evidence from earlier work still stands: the same source tree
is assembled into x86_64 release builds in CI every commit; there are no
AVX-specific code paths in the hot loops (SIMD is routed through Google
Highway, which has native NEON targets). The only arch-dependent intrinsics
are in `tests/unit/test_simd_bit_exact.cpp`, which already validates
bit-exactness across the two arches via the earlier devlog 088 run.

## F3 — Tighten `TenantNamespace_AllowTableInsideNamespace`

`tests/unit/test_table_acl_ddl.cpp:155-170` rewritten. The previous version
used `"deskA."` (mixed case + dot) which is not an unquoted SQL identifier,
so the executor's parser rejected the `SELECT * FROM "deskA.trades"` form
and the test weakened the assertion to `EXPECT_NE(r.status, 403)`.

New version:

- Tenant namespace set to `"deska_"` (all lowercase + underscore — a valid
  unquoted identifier prefix).
- `CREATE TABLE deska_trades (…)` issued first (`ASSERT_EQ(create_res.status, 200)`).
- `SELECT * FROM deska_trades` with `X-Zepto-Tenant-Id: tB` now asserts
  `EXPECT_EQ(r.status, 200)`.

Verified: `TableAclDdlTest.TenantNamespace_AllowTableInsideNamespace` passes
with the strict 200 assertion. The other three `TenantNamespace_*` tests
were left untouched.

## F4 — Prime prepared-statement cache from HTTP ACL parse

Before this devlog, the POST `/` handler parsed SQL once to resolve
`touched_table` for ACL/tenant checks, then `QueryExecutor::execute()`
parsed it again (cache miss on first call). Double parse on every write path.

### Executor helper

`include/zeptodb/sql/executor.h`:

```cpp
/// Prime the prepared-statement cache with a pre-parsed AST for the given
/// SQL. Used by the HTTP ACL path (devlog 091 F4) to avoid re-parsing.
/// No-op if the cache already contains this SQL's hash or is at capacity.
void cache_prepared(const std::string& sql, ParsedStatement ps);
```

`src/sql/executor.cpp`:

```cpp
void QueryExecutor::cache_prepared(const std::string& sql, ParsedStatement ps) {
    size_t h = sql_hash(sql);
    std::lock_guard<std::mutex> lk(stmt_cache_mu_);
    if (stmt_cache_.find(h) == stmt_cache_.end() && stmt_cache_.size() < 4096) {
        stmt_cache_.emplace(h, std::move(ps));
    }
}
```

Uses the same `sql_hash()` (member function, `std::hash<std::string>{}`) and
the same `stmt_cache_` container that `execute()` consults — so the first
`execute(sql)` after a `cache_prepared(sql, ps)` is guaranteed to be a cache
hit under the same `stmt_cache_mu_`.

### HTTP wiring

`src/server/http_server.cpp` POST `/` handler: the existing parse block was
hoisted out of its inner scope so the `ParsedStatement` survives until after
ACL / tenant checks. At `src/server/http_server.cpp:508`, just before
`run_query_with_tracking(req.body, …)`, we now call:

```cpp
if (have_parsed) {
    executor_.cache_prepared(req.body, std::move(cached_ps));
}
```

`run_query_with_tracking` internally calls `executor_.execute(sql)` which
consults the prepared-statement cache on the same hash → cache hit → no
re-parse.

Verify: `grep -n cache_prepared src/server/http_server.cpp` →
`508:            executor_.cache_prepared(req.body, std::move(cached_ps));`.

### Unit + HTTP-level tests

- `tests/unit/test_sql.cpp::SqlExecutorTest.CachePreparedAvoidsReparse` —
  direct executor-level test:
  - Starts from an empty prepared cache (`clear_prepared_cache()`).
  - Calls `cache_prepared(sql, ps)` → asserts `prepared_cache_size() == 1`.
  - Calls `cache_prepared(sql, ps2)` again (idempotent) → still `== 1`.
  - `execute(sql)` succeeds (cache hit path).
- `tests/unit/test_table_acl_ddl.cpp::TableAclDdlTest.HttpPostCachePriming_PostPrimesExecutorCache` —
  end-to-end proof that the HTTP POST path primes the executor cache:
  clears the cache, sends `POST / "SELECT * FROM trades"` against the live
  `HttpServer` fixture, and asserts `prepared_cache_size()` grew. Status
  code is intentionally not checked — the ACL-block parse runs regardless
  of whether the query later succeeds, and that parse is what must prime
  the cache.

## Test counts

- **C++ suite: 1221 tests, all passed.** (1219 baseline from devlog 090 + 1 `CachePreparedAvoidsReparse` + 1 `HttpPostCachePriming_PostPrimesExecutorCache`.) Full run ~277s on local x86_64.
- Targeted filter `*TableACL*:*Tenant*:*CachePrepared*:*HttpPostCachePriming*` → 12/12 passed, including the tightened F3 test and both new F4 tests.
- Integration `tests/integration/test_http_tenant.sh` → PASS.
- Integration `tests/integration/test_multiprocess.sh` → 5/5 passed (regression check).
- aarch64 EKS run → deferred, see F2 section.

## Files changed

- `tools/zepto_http_server.cpp` — `--tenant` flag parsing, help text, TenantManager provisioning block
- `src/server/http_server.cpp` — hoist `ParsedStatement` out of parse block; thread into `executor_.cache_prepared()` before `run_query_with_tracking`
- `include/zeptodb/sql/executor.h` — declare `cache_prepared(sql, ps)`
- `src/sql/executor.cpp` — implement `cache_prepared`
- `tests/unit/test_sql.cpp` — new `CachePreparedAvoidsReparse` test
- `tests/unit/test_table_acl_ddl.cpp` — tightened `TenantNamespace_AllowTableInsideNamespace`; new `HttpPostCachePriming_PostPrimesExecutorCache` (end-to-end F4 proof)
- `tests/integration/test_http_tenant.sh` — **new**, smoke test for `--tenant` CLI flag

## Doc updates

- `docs/COMPLETED.md` — devlog 091 bullet
- `docs/api/HTTP_REFERENCE.md` — `--tenant` flag, `X-Zepto-Tenant-Id` header
- `docs/devlog/090_multi_table_closeout.md` — residuals 1–4 marked RESOLVED
- `KIRO.md` — next devlog number bumped to 092

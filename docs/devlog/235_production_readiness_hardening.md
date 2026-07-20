# 235: Production Readiness Hardening

Date: 2026-07-20
Status: Implementation complete; production promotion gated

## Context

The production review after merging Physical AI Experiments 024-035 into
`dev` found security and bounded-materialization gaps that were not covered by
the earlier promotion tests. The Web console also carried a vulnerable Next.js
patch release, and the public documentation sync had no explicit classification
for the newest research evidence.

## Changes

### SQL authorization boundary

- Classify parsed SQL statements at both `POST /` and `GET /?query=`. POST
  requires `READ`, `WRITE`, or `ADMIN` according to statement type; GET is
  restricted to `SELECT`, `SHOW TABLES`, and `DESCRIBE` and returns 405 for
  DML/DDL to prevent cookie-authenticated cross-site GET mutations.
- Fail closed for malformed or unclassified SQL when authentication is enabled.
- Apply table allowlists and tenant namespaces to every real table reached
  through JOINs, CTEs, subqueries, set operations, and materialized-view
  definitions.
- Remove caller-provided internal identity and ACL headers before installing
  the authenticated context, preventing role or tenant spoofing.
- Propagate an API key's persisted `tenant_id` into `AuthContext` and audit SQL
  authorization denials.

### Distributed materialization and placement

- Snapshot decoded `STRING` and `SYMBOL` result values before stopping a
  coordinator-local temporary pipeline.
- Enforce JOIN and full-data/window row and byte budgets cumulatively while
  nodes are fetched, using only the remaining rows plus one overflow probe.
- Bound remote SQL response payloads before allocating their receive buffers;
  rejected connections are not returned to the socket pool.
- Make catalog persistence transactional for runtime placement changes: a save
  failure returns an error and restores the previous catalog and router state.
- Parse catalogs into a temporary registry and atomically install only a fully
  validated document. Existing but malformed, truncated, duplicate, or
  out-of-range catalogs now fail startup instead of being treated as absent.
- Persist catalogs through a unique temporary file, `fsync` the file, atomically
  rename it, and `fsync` the parent directory before reporting success.
- Roll back `CREATE TABLE` when a durable schema catalog cannot be saved, while
  defining `PURE_IN_MEMORY` catalog persistence as a successful no-op.
- Serialize schema-catalog transactions across CREATE, DROP, ALTER, and table
  placement so a failing rollback cannot overwrite a concurrent successful
  mutation. DROP and ALTER restore the exact prior schema on save failure;
  placement updates mutate the router only after every local catalog saves.

### RDB snapshot integrity and graceful restart

- Default post-flush arena reclamation off and make tiered pipelines reject an
  explicit reclaim or memory-limit eviction configuration until general SQL
  can merge arbitrary HDB-only partitions. This retains flushed RDB rows for
  query correctness instead of leaving dangling/reset column buffers.
- Write RDB snapshots to immutable generation directories. Validate every
  partition's column types and equal row counts under its write lock, write
  columns through unique temporary files, add a complete manifest with exact
  partition/row totals, and only then atomically replace `CURRENT`. Keep the
  prior generation and ignore unreferenced partial generations.
- Make graceful `ZeptoPipeline::stop()` stop workers, drain queued ingest, and
  publish a final generation, returning failure when publication fails. The
  production HTTP server stores this state under
  `<hdb-dir>/_rdb_snapshots`, enables recovery, and treats final snapshot
  failure as an unsuccessful shutdown.
- Harden HDB reads against invalid types/compression, overflow, truncated or
  trailing payloads, row-count/element-size disagreement, and table-scope
  mismatch before exposing mapped spans.
- Recover only the generation named by `CURRENT`, before worker startup. Check
  path components, required schema columns, exact types/row counts, partition
  routing keys, and manifest totals; detected structural corruption in the
  referenced generation aborts startup. Replay legacy ticks and schema-backed
  numeric/float/timestamp/bool rows into RDB so normal `QueryExecutor` SQL sees
  them after restart.
- Preserve the product boundary: dictionary-backed tables fail recovery until
  `StringDictionary` metadata is durable; abrupt node loss still lacks pipeline
  WAL replay, per-column checksums, and complete file/directory `fsync`;
  arbitrary HDB-only SQL merge remains open. No maximum data-loss window is
  claimed.
- Remove the stale Kubernetes failure-guide claims of automatic crash recovery,
  a 60-second RPO, and a nonexistent `/admin/snapshot` endpoint. The runbook now
  distinguishes a verified graceful stop from unbounded abrupt-loss exposure.

### Cluster RPC authentication

- Replace the replayable keyed-hash handshake with OpenSSL HMAC-SHA256 mutual
  challenge-response. Server and client proofs are domain-separated, bind a
  fresh nonce from each peer, and use constant-time comparison.
- Require exact authentication payload lengths, reject legacy one-message
  proofs, validate the server before sending the client proof, and close
  unexpected protocol responses instead of returning them to the socket pool.
- Apply a default 64 MiB client response ceiling to SQL, stats, metrics, and
  opaque binary RPC before receive-buffer allocation; bounded distributed
  materialization may impose a smaller per-query limit.
- Apply the configured payload ceiling to server responses as well. Oversized
  SQL results become a bounded error response, while oversized opaque results
  close the connection without a narrowing conversion or unbounded copy.
- Validate result column, row, cell, and decoded-string dimensions before
  vector allocation, and reject impossible/trailing wire payloads. Bound
  server socket writes by the graceful-drain timeout so a non-reading peer
  cannot pin a worker forever.
- Load cluster security by default in every TCP RPC client/server from exactly
  one of `ZEPTO_CLUSTER_SECRET_FILE` or `ZEPTO_CLUSTER_SECRET`; reject invalid,
  short, conflicting, unreadable, or OpenSSL-unavailable configurations.
- Make production cluster entry points fail closed when no secret is present:
  data and ingest nodes always require it, while the HTTP server requires it
  for HA, startup remote nodes, and runtime node additions. The explicit
  `--allow-insecure-cluster` override is limited to development.
- Document the remaining transport boundary: peer authentication is shipped,
  but TCP payload encryption and native mTLS are not; production deployments
  must keep RPC private or supply an encrypted overlay/service mesh.

### Production deployment defaults

- Make the standalone HTTP runtime fail closed: loopback is the default bind,
  non-loopback plaintext needs an explicit proxy-hop override, TLS files and
  authentication stores are preflighted, implicit development-key bootstrap is
  removed, rate limiting and audit recording default on, and request/query
  ceilings are configurable. Bind failures are reported synchronously instead
  of appearing as a successful background start.
- Order the system OpenSSL libraries before the pyarrow-wheel Arrow/Parquet
  fallback for every deployment executable through one CMake helper. Pyarrow exports private OpenSSL
  symbols; the previous ELF dependency order could interpose them into system
  `libssl` and crash while loading a server certificate. Real HTTPS and Flight
  TLS startup probes cover these paths.
- Execute HTTP and Flight boundary-authorized SQL from request-owned ASTs,
  bypassing shared statement/result caches. Full SQL text is now the ordinary
  cache key, and collision, concurrent-cap, cache-exhaustion, and unbounded
  result-cache priming regressions are covered.
- Enforce Flight tenant concurrency and cooperative read timeouts. Keep the
  non-atomic DoPut compatibility path disabled unless an explicit experimental
  opt-in is supplied. Plaintext overrides for Flight and bundled HTTP are
  independent.
- Fail closed for multi-node HTTP SELECT because the coordinator does not yet
  propagate request-owned bounds or cancellation across RPC. The explicit
  experimental override retains the research path with promotion blockers.
- Wire strict HTTP OIDC discovery/client/session configuration from CLI
  options. Prefer environment/file secret inputs, validate exact loopback
  redirect authorities and returned HTTPS metadata, and fail startup when
  discovery cannot activate. Parse explicit issuer ports correctly and verify
  the provider certificate through the configured system/custom CA trust.
- Mark identity, login, callback, session, refresh, and logout responses as
  `no-store, private` with legacy `Pragma: no-cache`, including authentication
  failures intercepted before route dispatch.
- Verify license JWTs as an exact three-part RS256 envelope without applying
  login-token-only identity claims to the separate license claim schema.
- Start and stop the pipeline with the listener lifecycle, reject an existing
  corrupt schema catalog before serving traffic, and validate tiered/cold
  storage paths. Tiered mode disables post-flush arena reclamation as a safety
  stopgap until general SQL is fully HDB-aware; this is not a durability claim.
- Automatically publish tiered graceful-shutdown RDB snapshots under
  `<hdb-dir>/_rdb_snapshots` and recover them before binding the listener.
  Detected structural corruption in the published generation is startup-fatal,
  and a failed final snapshot makes graceful process exit non-zero. Abrupt
  crash/WAL recovery remains open.

- Align release-candidate deployment metadata on application version `0.1.8`
  and bump the materially changed Helm package to `0.3.0`. Keep the image digest
  unset until publication, while allowing `image.digest=sha256:...` to select an
  immutable reference once the multi-architecture manifest exists.
- Keep normal release and Python-extension compilation portable instead of
  inheriting the build host's instruction set through `-march=native`;
  benchmark-only targets retain explicit native tuning.

- Remove `--no-auth` from both production Docker image defaults. The runtime
  now expects an explicit read-only API-key file and disables development-key
  bootstrap.
- Make the Helm default a single authenticated standalone replica behind a
  private `ClusterIP`; disable the HPA and reject unsafe standalone replica/HPA
  configurations at render time. Use an RWO-safe replacement strategy for the
  standalone PVC.
- Generate separate bootstrap API and cluster peer Secrets when operators do
  not supply existing Secrets. Preserve generated values across upgrades with
  `lookup`, mount only the hash-only API key store into the pod, and expose the
  raw bootstrap administrator key only through an explicit command in Helm
  NOTES. Generate a separate metrics-role credential for ServiceMonitor Bearer
  authentication; reject unauthenticated pod-annotation scraping.
- Mount cluster credentials through `ZEPTO_CLUSTER_SECRET_FILE` in every
  RPC-capable chart workload. Restore the missing StatefulSet data PVC mount,
  use the copied BusyBox binary for the distroless pre-stop hook, and add
  secret/config checksums to workload pod templates.
- Bind container workloads explicitly on `0.0.0.0`, disable synthetic startup
  ticks, and connect enabled PVCs to tiered storage with `--hdb-dir`. Because
  TLS terminates at an operator-managed ingress, explicitly permit plaintext
  only inside the isolated pod network and mark session cookies Secure. Apply
  the distroless UID/GID and `fsGroup` so provisioned volumes are writable
  without a privileged permission-fixing init container.
- Keep external TLS termination as an operator decision: the chart does not
  create an ingress, and it warns against external exposure until a
  TLS-terminating ingress/load balancer and network policy exist. RPC peer
  authentication still requires private or separately encrypted transport.
- Enable an ingress NetworkPolicy by default: same-namespace clients may reach
  HTTP, while cluster RPC/heartbeat ports accept only pods from the same Helm
  release. Expose explicit NetworkPolicy peers for approved cross-namespace TLS
  ingress or Prometheus workloads; note that policy isolation is not encryption.
- Remove default HugePages requests and mounts so a portable install does not
  remain Pending on nodes without pre-reserved pages. Latency profiles must opt
  in with matching HugePages requests/limits after node-capacity verification.
- Disable automatic service-account token mounting and drop all Linux
  capabilities with privilege escalation disabled. A read-only root filesystem
  remains subject to a separate writable-path audit for JIT/DuckDB temporary
  files and audit logs before it can be enabled safely.
- Stop presenting a mounted PVC as durable storage. Portable defaults are now
  in-memory, while enabling tiered storage requires an explicit evaluation
  acknowledgement and prints a warning. Graceful SQL row restart now uses the
  complete RDB snapshot path, while hot-partition WAL/abrupt recovery and full
  HDB-row merge into general SQL reads remain product-promotion blockers.
  The native CLI, like the chart, requires an explicit incomplete-durability
  acknowledgement before any tiered/cold-storage startup.
- Default the optional stateless ingest tier to fail closed. The current
  ingest binary does not load the shared API-key store, so authenticated ingest
  remains a promotion blocker; Helm rejects it unless an isolated benchmark
  explicitly enables no-auth and supplies storage routes.
- Reject HPA rendering in standalone and cluster modes. Cluster startup still
  uses a static ordinal peer list, so an HPA-created member would have an
  incomplete topology until dynamic discovery is implemented.
- Apply equivalent fail-closed auth, cluster-secret, single-replica, RWO, and
  `ClusterIP` defaults to the legacy Kubernetes manifest.

### Web and documentation release boundary

- Upgrade `next` and `eslint-config-next` from 16.2.1 to 16.2.10.
- Keep Experiments 031-035 and their complete Markdown evidence chain internal
  until a separate public-claim, raw-artifact-link, and route review approves
  publication. This site policy shipped independently in
  `ZeptoDB/zeptodb.github.io` PR 11.

## Product Boundary

These changes harden the already promoted bounded operational-table JOIN and
window scopes. They do not promote general distributed JOIN/window planning or
the experimental table-placement policy. The Physical AI experiment results
remain research-only and are not evidence of risk-free actions or physical
safety.

### Experimental Boundary

Two runtime paths remain available only behind explicit opt-ins:

- `--allow-experimental-distributed-queries` is intended for isolated,
  bounded cluster research. It has per-RPC frame ceilings but no end-to-end
  global row budget, distributed cancellation, or complete success/rejection
  telemetry. Failure is surfaced to the caller; removing the flag rolls back
  to the production default, which rejects multi-node HTTP SELECT with `503`.
- `--allow-non-atomic-put` is intended for disposable, idempotent Flight ingest
  compatibility tests within the documented row/byte caps. It does not provide
  atomicity, safe retry, crash recovery, or persistence guarantees. Failure can
  leave earlier rows committed; removing the flag disables DoPut.

Neither path is persisted as product state. Promotion requires the row/memory/
latency/concurrency limits, telemetry, fault tests, restart behavior, and
security evidence listed in `docs/BACKLOG.md`.

## Verification

The portable x86_64 production-feature build passes for every release binary
and the Python extension. CTest passes 1,902/1,902 registered tests, including
live HTTP/OIDC/TLS, Flight TLS/default-DoPut rejection, and ELF OpenSSL provider
ordering; the credential-gated live S3 upload test is the single intentional
skip. The focused HDB suite passes 22/22, including write → graceful stop →
destroy/restart → general SQL `COUNT`/`SUM`; the broader partition/storage/
pipeline/table-scoped filter passes 89/89 and the flush/cold-tier/tiered/
recovery filter passes 10/10. The live `test_http_hdb.sh` process restart also
passes catalog and general SQL row recovery.

The Python suite passes 408/408. The Web gate passes 83/83 tests, lint, the
Next.js production build, and the full `pnpm audit` with no known
vulnerabilities. Patched overrides cover vulnerable transitive development
dependencies as well as production dependencies.
Thirteen offline Helm security tests pass; the chart lints strictly, default,
existing-secret, and three-node cluster profiles render and pass Kubernetes
client dry-run, and the legacy manifest passes client dry-run. `actionlint`
passes every workflow, and the release-version dry run covers all six version
targets. Documentation-site QA and the internal-experiment publication boundary
shipped separately in site PR 11.

Hosted-runner triage also removed unavailable distro-level PyArrow packaging,
installs Python binding and Flight dependencies before configure/CTest, makes
structured logging fall back to stdout on a read-only log path, and isolates
the DDL replication test with kernel-assigned ports. The CXL latency-injection
regression now verifies a measurable configured minimum without imposing a
shared-runner performance ceiling; upper-bound latency remains benchmark
evidence rather than a unit-test correctness condition. The remaining RPC
metrics tests now use the same kernel-assigned port map as the rest of the
coordinator suite and assert successful server startup before connecting. The
focused local CTest selection for the logger fallback, parallel HTTP cluster
paths, HTTP production CLI, and Flight TLS passes 13/13.

Hosted ARM64 repeats the full production-profile build and CTest suite in the
required `Production gate`; cross-architecture completion is not claimed from
this x86_64 host. Main/prod promotion also remains subject to the repository
approval rule and the operator decisions listed in the deployment runbook.

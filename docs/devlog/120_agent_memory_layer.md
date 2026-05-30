# 120: Agent Memory Layer

Date: 2026-05-26
Status: Complete

## Context

ZeptoDB is expanding from a high-throughput in-memory time-series engine into a
real-time memory substrate for AI agents. The first additive wedge is agent
working memory, context retrieval, and application-level LLM cache lookup. The
goal is to differentiate from vector databases by keeping ZeptoDB's memory-first
latency profile, tenant/session filtering, and context assembly close to the
existing HTTP and Python surfaces.

## Changes

- Added `zeptodb::ai::AgentMemoryStore` with memory put/get/search,
  context assembly, exact cache lookup, semantic cache lookup, TTL filtering,
  pinned/importance/recency/access-count ranking, and embedding validation.
- Added the `zepto_ai` static library and linked it into the HTTP server and test
  target.
- Added HTTP endpoints under `/api/ai/*` for memory writes, memory search,
  context retrieval, cache store, cache lookup, and aggregate memory stats.
- Added Agent Memory sidecar persistence: `records.bin` for metadata and
  `vectors.bin` for row-major `float32` embeddings. `zepto_http_server` accepts
  `--agent-memory-dir`; when HDB is enabled, the default path is
  `<hdb-dir>/agent_memory`. Persistence now supports a mutation-count flush
  threshold via `--agent-memory-flush-every` and always force-flushes during
  server stop.
- Optimized memory search to retain a bounded top-K heap instead of sorting all
  matched candidates, keep normalized vectors in a row-major arena, and
  parallelize large exact full-scan searches while preserving exact top-K
  results.
- Optimized write-time TTL cleanup by tracking whether memory/cache entries have
  expiry timestamps. Non-TTL bulk loads now skip the expired-entry scan instead
  of rescanning the whole store after every write.
- Added bounded memory/cache eviction with `AgentMemoryEvictionConfig`,
  expired-entry cleanup, pinned-memory protection, and CLI capacity knobs
  `--agent-memory-max-memories` / `--agent-memory-max-cache-entries`.
- Added Agent Memory observability: `GET /api/ai/stats` and Prometheus metrics
  for memory/cache counts, embedding dimension, eviction counters, and configured
  capacity limits.
- Added `bench_agent_memory` for search, context assembly, exact cache, semantic
  cache, and sidecar save/load measurements.
- Added optional sparse-projection ANN candidate indexing for Agent Memory
  search/context. The index is partitioned by `(tenant_id, namespace)`, rebuilt
  from live vectors as derived state, and falls back to filtered scan when it
  cannot produce enough filtered candidates.
- Added `zepto_py` helpers exposed as `connection.memory` and `connection.cache`.
- Added Python examples under `examples/agent_memory/` for provider-cache and
  LangGraph-style retrieve/call/remember flows. The examples use mock providers
  and deterministic client-side embeddings, so they run without external API
  keys.
- Added optional example adapters for OpenAI Responses, Anthropic Messages, and
  LangGraph `StateGraph`. The adapters lazy-import provider/framework SDKs and
  require explicit model names from application config.
- Added a production-shaped agent demo that combines context retrieval,
  exact/semantic cache lookup, provider invocation on miss, memory write-back,
  and AgentOps telemetry inserts in one turn flow.
- Added AgentOps telemetry schema helpers for `agent_runs`,
  `retrieval_events`, `cache_events`, `llm_calls`, and `tool_calls`.
- Extended `bench_agent_memory` with sweep mode, configurable sweep record
  counts, `--ann off|auto|sparse_projection`, ANN rebuild timing, fallback
  counts, and an ANN decision summary that flags when search/context p50 exceeds
  a threshold.
- Added C++ unit coverage for memory ranking, token budget selection, invalid
  embeddings, TTL, tenant filtering, eviction, exact/semantic cache, concurrency,
  and HTTP error paths, plus store and HTTP-server persistence round trips and
  deferred stop-time flush behavior.
- Added Python client, example, and adapter tests for the new wrapper methods
  and example control flow.
- Documented the design, HTTP API, Python API, and C++ API.

## Verification

- `ninja -j$(nproc) zepto_tests zepto_http_server bench_agent_memory`
- `./tests/zepto_tests --gtest_filter='AgentMemory*'`
- `PYTHONDONTWRITEBYTECODE=1 python3 -m pytest -q tests/python/test_agent_memory_client.py tests/python/test_agent_memory_examples.py`
- `./bench_agent_memory --records 10000 --dim 128 --iters 50`
- `./bench_agent_memory --sweep --sweep-records 1000,2000 --dim 16 --iters 2 --skip-snapshot --ann-threshold-ms 10`
- `./bench_agent_memory --sweep --dim 128 --iters 5 --ann sparse_projection --ann-min-records 1 --ann-max-candidates 50000 --ann-threshold-ms 10`

Benchmark sample on the local build: 10K records, 128-dimensional embeddings,
50 iterations:

| Operation | p50 | p95 |
|---|---:|---:|
| memory search top-K | 1.23 ms | 1.40 ms |
| context assembly | 1.34 ms | 1.41 ms |
| exact cache lookup | 0.00 ms | 0.00 ms |
| semantic cache lookup | 0.07 ms | 0.07 ms |

Snapshot save was 5.79 ms, load was 11.60 ms, and the snapshot footprint was
804.2 bytes per memory item in this benchmark shape.

Current-instance exact scan, using 128-dimensional embeddings and sidecar
snapshot save/load disabled for search-only comparison:

Environment:

- 8 vCPU Intel Xeon 6975P-C
- 30 GiB RAM
- 16 GiB `/tmp`

| Records | Search p50 | Search p95 | Context p50 | Context p95 | Semantic cache p50 | ANN decision |
|---:|---:|---:|---:|---:|---:|---|
| 100K | 8.35 ms | 9.76 ms | 6.73 ms | 6.82 ms | 0.07 ms | scan-ok |
| 300K | 6.53 ms | 6.63 ms | 6.41 ms | 6.42 ms | 0.07 ms | scan-ok |
| 1M | 16.70 ms | 16.74 ms | 16.73 ms | 16.87 ms | 0.07 ms | candidate |

The 300K exact scan result improved from 29.69 ms search / 30.31 ms context to
6.53 ms search / 6.41 ms context after the parallel exact-scan change. A 1M
exact run before the TTL cleanup optimization measured 100.67 ms search /
100.57 ms context and a later post-scan-tuning attempt was stopped after fixture
load exceeded 17 minutes. After skipping write-time expired scans for non-TTL
records, the same 1M exact benchmark completes in 1.63 seconds total and reports
16.70 ms search / 16.73 ms context.

Sparse-projection ANN was also compared on the same instance with deterministic
128-dimensional random embeddings:

| Records | ANN config | Search p50 | Context p50 | ANN rebuild | Recall@16 | Decision |
|---:|---|---:|---:|---:|---:|---|
| 100K | 4 tables, 4K candidates, oversample 16 | 0.75 ms | 1.23 ms | 133.01 ms | 0.062 | fast but low recall |
| 100K | 16 tables, 50K candidates, oversample 1024 | 15.64 ms | 27.75 ms | 183.53 ms | 1.000 | high recall but slower than scan |
| 300K | 4 tables, 4K candidates, oversample 16 | 0.79 ms | 1.37 ms | 412.24 ms | 0.062 | fast but low recall |
| 1M | 4 tables, 4K candidates, oversample 16 | 0.95 ms | 1.44 ms | 1229.99 ms | 0.062 | fast but low recall |
| 1M | 16 tables, 50K candidates, oversample 1024 | 29.46 ms | 43.67 ms | 1826.20 ms | 0.125 | slower than scan, still low recall |

The practical conclusion is that ZeptoDB's default retrieval path should remain
parallel exact scan for v0. Sparse projection remains useful as a derived,
tenant-partitioned candidate-generation baseline, but current random-vector
tests show that it is not robust enough to promote as the recall-sensitive
default. HNSW/IVF or another stronger ANN family remains the right follow-up for
million-memory deployments.

## Follow-ups

- Add snapshot latency and failure metrics.
- Use the devlog 121 compare harness to evaluate HNSW/IVF candidates against
  exact scan and sparse-projection baselines.
- Evaluate HNSW/IVF or another ANN family against the parallel exact-scan and
  sparse-projection baselines using recall, write/update cost, memory overhead,
  and tenant-filter behavior.
- Add tenant/namespace-specific eviction limits if multi-tenant memory pressure
  requires fair sharing.
- Add vertical production examples that pair provider adapters with live
  time-series tables and Agent Memory context.
- Add context trace/replay for explainability.
- Keep server-managed embedding providers out of the default path; consider as an
  optional enterprise feature.

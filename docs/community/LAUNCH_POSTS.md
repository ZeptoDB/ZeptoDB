# Launch Post Drafts

Ready-to-post content for Reddit, Hacker News, and other platforms.

> Last updated: 2026-05-28 (website-first Agent Memory positioning).
> Numbers verified against `README.md`, `docs/COMPLETED.md`, `docs/devlog/099_ingest_path_recovery.md`.
> License: **BSL-1.1**, Change Date 2030-04-01 (then Apache-2.0). Additional Use Grant permits production use; commercial DBaaS resale is restricted until the Change Date.

---

## Current launch angle — Agent Memory for live time-series

Use this first. The goal is to send people to the website, not make them parse the whole GitHub README.

Primary links:

- Website: https://zeptodb.com
- Agent Memory: https://zeptodb.com/use-cases/agent-memory/
- Python quickstart: https://zeptodb.com/use-cases/agent-memory-python-quickstart/
- Why Agent Memory Needs Time-Series: https://zeptodb.com/blog/why-agent-memory-needs-time-series/
- Benchmarks: https://zeptodb.com/benchmarks/
- Agent Memory vs Vector Databases: https://zeptodb.com/compare/agent-memory-vs-vector-databases/
- GitHub: https://github.com/ZeptoDB/ZeptoDB

### Short X / LinkedIn post

```text
Most agent memory systems store summaries or embeddings without the live event stream that made those memories true.

ZeptoDB takes a different path: microsecond time-series evidence + tenant-scoped Agent Memory + exact/semantic prompt cache + AgentOps telemetry on one replayable timeline.

5.52M events/sec ingest
272us query on 1M rows
1.23ms filtered memory search
522ns Python zero-copy

Website: https://zeptodb.com
Agent Memory guide: https://zeptodb.com/use-cases/agent-memory/
```

### Hacker News / Reddit title options

1. `ZeptoDB: Agent memory for live time-series data`
2. `Why agent memory needs time-series data`
3. `I built an agent memory layer on top of a microsecond time-series engine`
4. `Agent memory is weaker without the event stream that made it true`

### HN / Reddit body

```markdown
I've been building ZeptoDB, an in-memory time-series database in C++20.

The original goal was kdb+-class time-series performance with standard SQL and
zero-copy Python. The newer direction is Agent Memory for operational agents:
keep live evidence, retrieved memories, prompt cache hits, model calls, tool
calls, and decisions on one replayable timeline.

Why this matters:

Most agent memory systems store summaries or embeddings without the live event
stream that made those memories true. That works for chat recall, but it is
weak for agents attached to factories, robots, trading systems, fleets, grids,
or observability pipelines.

An operational agent needs to answer:

- What changed before the alert?
- Which raw evidence was available?
- Which memories were retrieved?
- Was the response cached or did we call a model?
- Which tool was called?
- What happened afterward?

Those are time-series questions, not just vector-search questions.

Current numbers:

- 5.52M events/sec single-node ingest
- 272us query on 1M rows
- 522ns query result to Python/NumPy
- 1.23ms filtered memory search over 10K 128-dim records
- exact and semantic prompt cache lookup

What ZeptoDB does not do: it does not call LLM or embedding providers from the
server. Applications own prompts, models, provider credentials, and embeddings.
ZeptoDB owns storage, filtering, ranking, cache lookup, context assembly,
telemetry, and time-series replay.

Website: https://zeptodb.com
Agent Memory guide: https://zeptodb.com/use-cases/agent-memory/
Benchmarks: https://zeptodb.com/benchmarks/
GitHub: https://github.com/ZeptoDB/ZeptoDB
```

### First comment / follow-up

```markdown
Two useful pages if you want the product shape without reading the whole repo:

- Why Agent Memory Needs Time-Series Data:
  https://zeptodb.com/blog/why-agent-memory-needs-time-series/
- Agent Memory vs Vector Databases:
  https://zeptodb.com/compare/agent-memory-vs-vector-databases/

The honest boundary: Agent Memory v0 is single-node. In a clustered deployment,
route `/api/ai/*` to a sticky pod or treat the memory layer as a per-pod cache
until cluster-consistent memory routing lands. The time-series cluster remains
distributed.
```

---

## Phase 1 — Reddit first post (r/algotrading)

**Why r/algotrading first:** practitioners who feel the kdb+ license pain directly. Stronger first-post ROI than r/programming (too big, mods nuke promo posts) or r/cpp (audience scrutinises perf claims hard before engaging).

### Title options

1. **I got tired of paying kdb+ license fees, so I rewrote the tick engine in C++ (open source, ASOF JOIN included)** ← recommended
2. **Built an open-source kdb+ alternative on weekends — 5.52M ticks/sec, standard SQL**
3. **After 2 years on a kdb+ desk, I built a free alternative with standard SQL. Here's what I learned.**

### Body (~470 words)

```markdown
I worked on quant infra for two years. Two things drove me crazy:

1. The kdb+ license. ~$100K/core/year for production. Hard to justify when
   you're not at a top-5 fund.
2. The q language. Every new hire spent 2 months learning it before shipping
   anything. That's expensive in engineer-time, and it locked our codebase
   into a tiny hiring pool.

I tried the obvious alternatives before building anything.

ClickHouse is great for analytics, but it doesn't have ASOF JOIN. If you've
never used ASOF JOIN, it's the SQL operator that lets you do tick-by-tick
correlation across feeds — joining a trade with the most recent quote at or
before its timestamp. You can fake it with correlated subqueries but it's
slow and ugly.

InfluxDB chokes above ~500K events/sec per series. TimescaleDB is fine for
slower workloads but not for tick data.

So I started writing my own thing in C++ on weekends. It became ZeptoDB.

**What it does**

- Standard SQL with ASOF JOIN, Window JOIN, xbar (kdb+-style time
  bucketing), VWAP, EMA — the financial functions you actually use
- 5.52M ticks/sec sustained single-node ingest (8 cores, x86)
- 272µs filter on 1M rows, 248µs GROUP BY
- FIX (350ns), NASDAQ ITCH (250ns), Kafka, MQTT, OPC-UA native consumers
- Python zero-copy bridge — DataFrame in, DataFrame out, no serialization
- Source-available (BSL-1.1, becomes Apache-2.0 in 2030), self-host, K8s
  Helm chart included
- x86 and ARM/Graviton both supported (test matrix runs on both)

**What surprised me building it**

The wins came from places I didn't expect.

- Highway SIMD on window aggregates: 11x over scalar
- LLVM JIT on filter predicates kept us within kdb+'s range on most queries
- Per-(table, symbol, hour) partition keys gave 2–50x speedup on multi-table
  workloads. We started with a symbol-only key and it caused weird
  cross-table data leaks until we found it.

The thing that took longest wasn't performance. It was distributed cluster
correctness — split-brain defense, FencingToken in the RPC header, K8s
Lease integration, online partition rebalancing. Tick data needs strong
correctness guarantees and most of the engineering effort went there, not
into making queries fast.

**What it's not (yet)**

Things I'd rather you know up front than hit in production:

- No JDBC/ODBC drivers. Tableau works through a ClickHouse protocol shim,
  Excel doesn't.
- No managed cloud. Self-host only for now.
- Window functions over virtual tables aren't supported.
- One query (VWAP 1M p50) has a ~7% gap vs my best baseline due to a
  clang register-spill issue. Documented in the devlog if you care.

**Where it ended up**

Started for quants. The same engine now runs in semiconductor fabs (10kHz
OPC-UA sensor data), game backends (Kafka telemetry, anti-cheat
analytics), and physical AI sensor fusion (ASOF JOIN across LiDAR + camera
+ IMU). Different verticals, same workload shape.

Happy to answer questions — the kdb+ comparison, why C++ over Rust, why I
didn't just put q on top of a free DB, anything.

GitHub: https://github.com/ZeptoDB/ZeptoDB
Site: https://zeptodb.com
```

### Pre-write FAQ answers (have these ready before posting)

| Likely question | Answer outline |
|---|---|
| **How does it compare to kdb+ on Y?** | Honest, with numbers. Acknowledge where kdb+ still wins (mature ecosystem, q.io tooling, decades of war-tested operations). Cite our specific gap (VWAP 1M p50 ~7% behind baseline). |
| **Production-ready?** | "Used internally by [X] for [Y workload]. I'd call it beta for general use. Cluster correctness is the most-tested part; HA + WAL replication has explicit fencing/quorum tests." |
| **Why not Rust?** | "Started in C++ in 2024 because the SIMD (Highway) and LLVM JIT ecosystems were more mature there. Rust would be my pick if I started today." |
| **Why didn't you put q on top of a free DB?** | "Because the q parser is the easy part — the hard part is ASOF JOIN, vectorised aggregates, and 5M/sec ingest. Those are engine-level problems that q-on-Postgres can't solve." |
| **Why open source if you want to make money?** | "OSS engine + Enterprise tier (RBAC, audit log, cluster HA, priority support). Same model as ClickHouse Inc. and Elastic before them." |
| **What's the license trap?** | "BSL-1.1 with a 2030-04-01 Change Date to Apache-2.0. Additional Use Grant lets you run it in production today. The restriction is on reselling it as a commercial managed DBaaS until the Change Date — same model as MariaDB, Sentry, CockroachDB before they went pure-OSS." |
| **Benchmarks reproducible?** | "Yes — `tools/run-full-matrix.sh` runs the full bench suite on x86 + Graviton in parallel. Numbers in the post are clang-19 medians. Methodology in `docs/bench/`." |
| **Does it work with Grafana / DataGrip / DBeaver?** | "Grafana yes (ClickHouse-compatible HTTP API). DataGrip/DBeaver via the same shim, partial. Native ClickHouse wire protocol is on the backlog." |
| **What about Physical AI / robotics?** | "OPC-UA connector is SLA-grade (Basic256Sha256 security, reconnect with backoff, quality mapping). Sector profiles for semiconductor fab, automotive, steel, generic. ROS2 plugin is on the backlog." |
| **Single point of failure?** | "Cluster HA with K8s Lease + RingConsensus. CoordinatorHA standby/active with FencingToken in RPC header to prevent split-brain. WAL quorum replication." |

### Things to have screenshot-ready (for comments, not the post)

- `EXPLAIN` output of an ASOF JOIN query
- Benchmark comparison table (ZeptoDB vs kdb+ vs ClickHouse vs InfluxDB)
- A 10-line SQL example that uses xbar + VWAP + ASOF JOIN
- Helm install one-liner output

---

## Phase 2 — Show HN (after Reddit lands)

Post 1–2 weeks after the Reddit launch, once messaging is dialed in based on Reddit feedback.

**Title:** `Show HN: ZeptoDB – Open-source time-series DB for Physical AI, Finance, IoT (5.52M events/sec)`

**Body:**

```markdown
Hi HN,

I've been building ZeptoDB, an in-memory columnar time-series database in
C++20. It started as a kdb+ alternative for tick data, but the same engine
now runs in semiconductor fabs (OPC-UA sensors), game backends (Kafka
telemetry), and Physical AI sensor fusion.

Numbers (single node, 8 cores x86):
- Ingestion: 5.52M ticks/sec sustained
- Filter 1M rows: 272µs
- GROUP BY 1M rows: 248µs
- SQL parse: 1.5–4.5µs
- Python zero-copy column access: 522ns
- ARM/Graviton perf parity (test matrix runs on both)

What makes it different:
- Highway SIMD in the hot path (11x on window aggregates)
- LLVM JIT for filter predicates
- Lock-free MPMC ring buffer for ingest
- ASOF JOIN, Window JOIN, xbar — temporal operators are first-class
- Standard SQL over HTTP (ClickHouse-compatible, so Grafana works)
- Zero-copy Python/numpy/Polars bridge
- Per-(table, symbol, hour) partition keys (2–50x speedup on multi-table
  workloads)
- Distributed cluster: consistent-hash routing, K8s Lease + FencingToken
  for split-brain defense, WAL quorum replication

Built originally for finance (tick data, VWAP, EMA), but the workload shape
generalises — high-velocity time-series with strong temporal join semantics
shows up in factories, fleets, and game servers too.

License: BSL-1.1 with 2030 conversion to Apache-2.0. Production use is
explicitly granted; only commercial DBaaS resale is restricted until then.

GitHub: https://github.com/ZeptoDB/ZeptoDB
Site: https://zeptodb.com
Try it: `docker run -p 8123:8123 zeptodb/zeptodb:latest`

Happy to answer questions about the architecture, the SIMD/JIT path, or
the multi-industry pivot.
```

---

## Phase 3 — Other Reddit subs (stagger, 1 per 3–5 days)

Each sub gets the same engine but a different framing. **Do not cross-post.** Mods read each sub.

### r/quant

**Title:** `Open-source kdb+ alternative — ASOF JOIN, VWAP, EMA, xbar in standard SQL (1293 tests, BSL-1.1)`

Same body as r/algotrading but swap the opening pain point: "Hard to justify on a research budget where most of the data work is exploratory" instead of the trading-desk framing.

### r/databases

**Title:** `ZeptoDB — open-source time-series DB with ASOF JOIN, 5.52M events/sec ingest, ClickHouse-compatible HTTP API`

Lead with the database engineering angle:
- Storage: Arena → Column Store → HDB → Parquet → S3 tiering
- Query: cost-based planner, JIT, SIMD, parallel scatter-gather
- Cluster: consistent hashing, FencingToken, RingConsensus, partition rebalancing
- License: BSL-1.1 → Apache-2.0 (2030)

### r/cpp

**Title:** `ZeptoDB — C++20 time-series database with Highway SIMD, LLVM JIT, lock-free ring buffers (open source)`

Focus on implementation:
- Highway for portable SIMD (AVX2/AVX-512/NEON, single source)
- LLVM JIT for runtime predicate compilation
- Lock-free MPMC ring buffer with `drain_threads` auto-sizing
- Arena allocator (zero GC, zero fragmentation)
- NUMA-aware allocation
- pybind11 zero-copy bridge to numpy/Arrow
- 1293 unit tests, x86 + ARM/Graviton CI matrix

Expect tough questions on benchmark methodology; have `docs/bench/` ready.

### r/dataengineering

**Title:** `Building a 5M events/sec time-series DB: what worked and what didn't`

War-stories framing:
- The partition key bug that leaked data across tables (per-symbol → per-(table,symbol,hour))
- Why store_tick column-pointer caching was the difference between 4.66M and 4.87M ticks/sec
- Why we picked BSL-1.1 instead of Apache-2.0 outright

Engineering audience appreciates the post-mortem framing more than the announcement framing.

### r/MachineLearning + r/robotics (Physical AI angle)

**Title:** `Time-series DB with OPC-UA + ASOF JOIN for sensor fusion (LiDAR + camera + IMU)`

Lead with Physical AI:
- OPC-UA SLA-grade connector (Basic256Sha256, reconnect, quality mapping)
- Sector profiles: semiconductor fab, automotive, steel, generic
- ASOF JOIN aligns LiDAR/camera/IMU streams without re-sampling
- Parquet HDB for long-horizon training-set replay
- Used in [example workload]

### r/selfhosted

**Title:** `ZeptoDB — self-hosted time-series database, Docker one-liner, Grafana-compatible`

Focus on ops simplicity:
- Single docker command
- Grafana works natively (ClickHouse HTTP API)
- Web UI included (the console pages)
- Helm chart for K8s
- Built-in WAL replication if you go multi-node

---

## Engagement rules (apply to every post)

| Rule | Why |
|---|---|
| Be on the post for the first 2 hours | Comment velocity = visibility |
| Reply to every substantive comment within 30 min on launch day | Signals real human, not drive-by promo |
| Acknowledge criticism factually | "Good point, kdb+ does X better. Our approach was Y because Z." |
| Never delete a critical comment or downvote-bot it | Reddit detects this and brand-damages permanently |
| GitHub link only at the bottom of the post, never in comments | Repeat linking = self-promo flag |
| No alt-account comments | Detection ban = lifetime brand damage |
| Don't cross-post within 24 hours | Spam trigger |
| Match the sub's voice — read top-10 of last week before posting | Cultural fit > anything else |

## Words to avoid (Reddit instantly filters as marketing)

`revolutionary`, `next-generation`, `cutting-edge`, `game-changing`, `blazing fast`, `lightning-fast`, `ultra-low latency` (use specific µs numbers instead), `seamlessly`, `enterprise-grade` (without specifics), `world-class`, `state-of-the-art`.

## Timing strategy

| Phase | Sub | When | Day/time |
|---|---|---|---|
| 1 | r/algotrading | Week 1 | Tue/Wed 9–11am ET |
| 1.5 | r/quant | +3 days after r/algotrading | Tue/Wed 9–11am ET |
| 2 | r/databases | Week 2 | Wed 10am ET |
| 2 | r/cpp | Week 2 +3 days | Tue 10am ET |
| 3 | Show HN | Week 3 | Tue 7am PT |
| 3 | r/dataengineering | Week 3 +2 days | Tue 9am ET |
| 4 | r/MachineLearning + r/robotics | Week 4 | Wed 10am ET |
| 4 | r/selfhosted | Week 4 +2 days | Sat 10am ET (selfhosters post on weekends) |

Avoid: Friday afternoons, weekends for engineering subs, US holidays.

## Pre-launch checklist

- [ ] GitHub README has architecture diagram, badges, screencast/GIF demo
- [ ] Docker image works: `docker run -p 8123:8123 zeptodb/zeptodb:latest`
- [ ] Quick Start guide tested end-to-end (5 min from `docker run` to first query)
- [ ] zeptodb.com is live and matches messaging (post-devlog-115 rebrand: ✅ done; AuthGuard separation for public marketing pages: ⚠️ pending)
- [ ] docs.zeptodb.com is live
- [ ] Discord invite link working
- [ ] Pricing page license language matches actual LICENSE file (currently says "Apache-2.0-compatible" — should be "BSL-1.1, becomes Apache-2.0 in 2030" — see doc-drift note below)
- [ ] FAQ answers above are pre-written and saved
- [ ] Block off 2 hours immediately after posting for comment engagement

---

## Known doc-drift to fix before launch

| Surface | Current (wrong) | Should be |
|---|---|---|
| `web/src/app/(marketing)/pricing/page.tsx:31` | "Apache-2.0-compatible license" | "BSL-1.1 (Apache-2.0 from 2030-04-01)" or "source-available, becomes Apache-2.0 in 2030" |
| `docs/community/REGISTRY_SUBMISSIONS.md:43` | already says "BSL-1.1" ✅ | unchanged |
| `CONTRIBUTING.md:129` | already says "BSL-1.1" ✅ | unchanged |

The marketing copy must match the LICENSE file exactly. Reddit/HN crowd will read the LICENSE and call out any mismatch.

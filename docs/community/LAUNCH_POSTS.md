# Launch Post Drafts

Ready-to-post content for Hacker News, Reddit, and other platforms.

---

## Show HN Post

**Title:** `Show HN: ZeptoDB – Open-source in-memory time-series DB (5.52M events/sec, sub-ms queries)`

**Body:**

Hi HN,

I've been building ZeptoDB, an in-memory columnar time-series database written in C++20. The goal: handle both high-throughput ingestion and real-time analytics without trade-offs.

Key numbers (single node, no cherry-picking):
- Ingestion: 5.52M events/sec
- Filter 1M rows: 272μs
- VWAP 1M rows: 532μs
- SQL parse: 1.5–4.5μs
- Python zero-copy column access: 522ns

What makes it different:
- Highway SIMD for vectorized scans (not just marketing — it's in the hot path)
- LLVM JIT for expression evaluation
- Lock-free MPMC ring buffer for ingestion
- ASOF JOIN, Window JOIN, xbar — temporal operations are first-class
- Standard SQL over HTTP (ClickHouse wire-compatible, so Grafana works out of the box)
- Zero-copy Python/numpy/Polars integration
- Distributed cluster with consistent hashing, auto failover, WAL replication

Built for finance (tick data, VWAP, EMA), IoT (sensor streams), and observability. But it's a general-purpose TSDB.

The codebase is ~830 tests, runs on x86_64 and ARM Graviton. BSL-1.1 licensed (Apache 2.0 in 2030).

GitHub: https://github.com/zeptodb/zeptodb
Docs: https://docs.zeptodb.io
Try it: `docker run -p 8123:8123 zeptodb/zeptodb`

Happy to answer questions about the architecture, SIMD/JIT implementation, or benchmarking methodology.

---

## Reddit Posts

### r/programming

**Title:** `ZeptoDB: Open-source in-memory time-series DB — 5.52M events/sec ingestion, sub-millisecond queries (C++20, LLVM JIT, SIMD)`

Use the same body as Show HN, adjusted for Reddit formatting.

### r/cpp

**Title:** `ZeptoDB — C++20 time-series database using Highway SIMD, LLVM JIT, lock-free ring buffers`

Focus on C++ implementation details:
- Arena allocator (zero GC, zero fragmentation)
- Highway SIMD for portable vectorization
- LLVM JIT for runtime expression compilation
- Lock-free MPMC ring buffer
- NUMA-aware memory allocation
- UCX/RDMA transport

### r/algotrading / r/quant

**Title:** `Open-source alternative to kdb+ for tick data — ASOF JOIN, VWAP, EMA, xbar in SQL`

Focus on finance use cases:
- kdb+ migration toolkit included
- ASOF JOIN for point-in-time lookups
- Window JOIN for time-range aggregation
- xbar for candlestick bars
- Zero-copy Python for backtesting

### r/selfhosted

**Title:** `ZeptoDB — self-hosted time-series database, single Docker command, Grafana-compatible`

Focus on ease of deployment:
- `docker run -p 8123:8123 zeptodb/zeptodb`
- ClickHouse-compatible API (Grafana works natively)
- Web UI included
- Helm chart for Kubernetes

---

## Timing Strategy

1. **Week 1:** Docker Hub image published, README renewed
2. **Week 2:** Show HN post (Tuesday or Wednesday, 9-11am ET)
3. **Week 2-3:** Reddit posts (stagger across subreddits, 1-2 per day)
4. **Week 3:** Awesome list PR, DB-Engines submission
5. **Ongoing:** Respond to every comment within 2 hours on launch day

## Launch Day Checklist

- [ ] Docker image works: `docker pull zeptodb/zeptodb && docker run -p 8123:8123 zeptodb/zeptodb`
- [ ] Quick Start guide tested end-to-end
- [ ] Docs site live at docs.zeptodb.io
- [ ] Website live at zeptodb.io (at minimum: landing page)
- [ ] GitHub README has badges, architecture diagram, GIF demo
- [ ] Discord invite link working
- [ ] Someone available to respond to comments for 12+ hours

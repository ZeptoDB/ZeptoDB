# APEX-DB Backlog

## ✅ kdb+ Gap Closure Complete! (95% Replacement Rate Achieved)

**Goal:** Core features without which kdb+ replacement is impossible
**Reference:** `docs/design/kdb_replacement_analysis.md`, `docs/devlog/010_financial_functions.md`

| Task | Status | Performance |
|------|--------|-------------|
| **xbar (time bar aggregation)** | ✅ | 1M → 3,334 bars in 24ms |
| **ema (exponential moving average)** | ✅ | 1M rows in 2.2ms |
| **LEFT JOIN** | ✅ | NULL sentinel (INT64_MIN) |
| **Window JOIN (wj)** | ✅ | O(n log m) binary search |
| **deltas/ratios native** | ✅ | OVER window functions |
| **FIRST/LAST aggregates** | ✅ | For OHLC candlestick charts |

**Result:** kdb+ replacement rate **HFT 95%, Quant 90%, Risk 95%**

---

## Immediate (Next Commit)
- [x] **Business strategy documentation** ✅ Completed (2026-03-22)
  - `docs/business/BUSINESS_STRATEGY.md` - Full business strategy (13 sections)
    - Market analysis, competitive strategy, product status, GTM strategy
    - Migration toolkit roadmap, financial projections, team buildout
    - 12-month goal: $3.6M ARR, 43 customers
  - `docs/business/EXECUTIVE_SUMMARY.md` - 1-page summary (for investors/executives)
- [x] **Full design document update** ✅ Completed (2026-03-22)
  - high_level_architecture.md: All layers current (SQL/HTTP/Cluster/parallel query/Python ecosystem)
  - initial_doc.md: General OLAP/TSDb targets, apex_py complete, test count 429+
  - system_requirements.md: FR-4 apex_py, FR-6 Migration Toolkit, FR-7 Production Ops
  - layer4: pybind11, apex_py full package documented
  - README.md: 429+ tests badge, devlog 013, Python ecosystem in Completed, SQL Phase 1
  - kdb_replacement_analysis.md: Parallel query, Parquet HDB, S3 Sink all reflected

## High Priority (Technical)
- [x] **SQL parser completion (Phase 1–3)** ✅ Completed (2026-03-22) — Core for attracting ClickHouse users
  - Phase 1: IN, IS NULL/NOT NULL, NOT, HAVING
  - Phase 2: SELECT arithmetic, CASE WHEN, multi-column GROUP BY
  - Phase 3: Date/time functions, LIKE/NOT LIKE, UNION/INTERSECT/EXCEPT
  - Parallel paths (`exec_agg_parallel`, `exec_group_agg_parallel`) synchronized with all Phase 2/3 features
  - Remaining: subqueries/CTE, RIGHT JOIN, EXPLAIN, SUBSTR (see SQL Completeness section)
- [x] **Time range index** ✅ Completed (2026-03-22)
  - `Partition::timestamp_range(lo, hi)` — O(log n) binary search within partition
  - `Partition::overlaps_time_range(lo, hi)` — O(1) partition skip check
  - `PartitionManager::get_partitions_for_time_range(lo, hi)` — O(partitions) key-based partition pruning
  - `QueryExecutor::exec_select()` — partition-level time range pre-filtering before dispatch
  - `QueryExecutor::eval_where_ranged()` — row scan restricted to binary-search result range
  - All three exec paths (simple_select, agg, group_agg) use timestamp binary search
  - 8 new tests: partial scan, agg, GROUP BY, empty range, single row, no-symbol filter
- [ ] **Graviton (ARM) build test** — r8g instance, Highway SVE

## High Priority (Business/Operations)
- [x] **Production deployment guide** ✅ Completed (2026-03-22)
  - `docs/deployment/PRODUCTION_DEPLOYMENT.md` - Bare-metal vs cloud selection guide
  - `scripts/tune_bare_metal.sh` - Bare-metal auto-tuning script
  - `Dockerfile` - Cloud-native image
  - `k8s/deployment.yaml` - Kubernetes deployment (HPA, PVC, LoadBalancer)
- [x] **Python ecosystem integration** ✅ Complete (2026-03-22)
  - `apex_py.from_polars(df, pipeline)` — zero-copy (polars .to_numpy() + ingest_batch())
    - df.slice() zero-copy chunking, Series.to_numpy() direct Arrow buffer reference
    - price_scale param: float64 price → int64 (e.g. ×100 = cents)
    - sym_col/price_col/vol_col custom column mapping
  - `apex_py.from_pandas(df, pipeline)` — vectorized (.to_numpy() + ingest_batch())
    - iterrows() completely removed → numpy batch path (100-1000x faster)
    - float64 price auto-handling via price_scale
  - `apex_py.from_arrow(table, pipeline)` — Arrow Table vectorized ingest
    - to_numpy(zero_copy_only=False) column extraction → single ingest_batch() call
    - null-safe handling (pc.if_else fill_null)
  - `ArrowSession.ingest_arrow_columnar()` — per-column Arrow array direct ingest
  - `apex.Pipeline.ingest_float_batch()` — float64 arrays directly accepted (C++ conversion)
  - 38 new tests added (`tests/python/test_fast_ingest.py`)
  - **Performance:** 1M rows from_polars ~0.3s (vs. iterrows ~30s+)
  - **Business value:** Seamless Jupyter research → production deployment
- [ ] **Limited DSL AOT compilation** — Production deployment & IP protection
  - **Phase 1 (1 week):** Nuitka integration - Python DSL → single binary (15x smaller)
  - **Phase 2 (1 month):** Cython support - core ops → C extensions (2-3x additional speed)
  - **Phase 3 (3-6 months):** Limited DSL transpiler - filter/select/groupby → auto SQL conversion
  - **Business value:** Customer production deployment ease + source code IP protection
- [x] **Production monitoring & logging** ✅ Completed (2026-03-22)
  - `/health`, `/ready`, `/metrics` endpoints
  - Prometheus OpenMetrics format
  - Structured JSON logging (spdlog)
  - Grafana dashboard + 9 alert rules
  - `docs/operations/PRODUCTION_OPERATIONS.md` operations guide
- [x] **Backup & recovery automation** ✅ Completed (2026-03-22)
  - `scripts/backup.sh` - HDB/WAL/Config backup, S3 upload
  - `scripts/restore.sh` - Disaster recovery, WAL replay
  - `scripts/eod_process.sh` - EOD process automation
  - cron: backup (02:00), EOD (18:00)
- [x] **Production service installation** ✅ Completed (2026-03-22)
  - `scripts/install_service.sh` - One-step installation
  - `scripts/apex-db.service` - systemd service
  - Auto-restart, CPU affinity, OOM protection
  - Log rotation (30 days)
- [x] **Feed Handler Toolkit (Full Version)** ✅ Completed (2026-03-22)
  - **Implementation (8 headers + 5 implementations):**
    - `src/feeds/fix_parser.cpp` - FIX protocol (350ns parsing)
    - `src/feeds/fix_feed_handler.cpp` - FIX TCP receiver (async, reconnect)
    - `src/feeds/multicast_receiver.cpp` - Multicast UDP (<1μs)
    - `src/feeds/nasdaq_itch.cpp` - NASDAQ ITCH 5.0 (250ns parsing)
    - `src/feeds/optimized/fix_parser_fast.cpp` - Optimized version (zero-copy, SIMD)
  - **Tests (27 unit + 10 benchmark):**
    - `tests/feeds/test_fix_parser.cpp` - 15 tests (100% coverage)
    - `tests/feeds/test_nasdaq_itch.cpp` - 12 tests (100% coverage)
    - `tests/feeds/benchmark_feed_handlers.cpp` - Performance validation
  - **Optimizations (6 techniques):**
    - Zero-copy parsing (2-3x), SIMD AVX2 (5-10x), Memory Pool (10-20x)
    - Lock-free Ring Buffer (3-5x), Fast number parsing (2-3x), Cache-line alignment (2-4x)
  - **Integration example:**
    - `examples/feed_handler_integration.cpp` - FIX/ITCH/performance test
  - **Documentation:**
    - `docs/feeds/FEED_HANDLER_GUIDE.md` - Usage guide
    - `docs/feeds/PERFORMANCE_OPTIMIZATION.md` - Optimization guide
    - `docs/feeds/FEED_HANDLER_COMPLETE.md` - Completion report
  - **Business value:** HFT market entry ($2.5M-12M), direct exchange connectivity, full kdb+ replacement
- [x] **Migration Toolkit** ✅ Completed (2026-03-22)
  - **kdb+ → APEX-DB** ✅ Complete
    - `include/apex/migration/q_parser.h` - AST architecture
    - `src/migration/q_lexer.cpp` - q language Lexer
    - `src/migration/q_parser.cpp` - q AST Parser
    - `src/migration/q_to_sql.cpp` - q→SQL Transformer (wavg/xbar/aj/wj)
    - `include/apex/migration/hdb_loader.h` + `src/migration/hdb_loader.cpp` - HDB splayed table loader (mmap-based)
    - `tools/apex-migrate.cpp` - CLI (query/hdb/clickhouse/duckdb/timescaledb modes)
    - `tests/migration/test_q_to_sql.cpp` - 20 tests
    - `tests/migration/test_hdb_loader.cpp` - 15 tests
  - **ClickHouse → APEX-DB** ✅ Complete
    - `include/apex/migration/clickhouse_migrator.h` + `src/migration/clickhouse_migrator.cpp`
    - DDL generation (MergeTree/LowCardinality/Gorilla codec), kdb+ type mapping
    - ASOF JOIN conversion, xbar→toStartOfInterval, FIRST/LAST→argMin/argMax
    - `tests/migration/test_clickhouse.cpp` - 18 tests
  - **DuckDB interoperability** ✅ Complete
    - `include/apex/migration/duckdb_interop.h` + `src/migration/duckdb_interop.cpp`
    - Parquet export (SNAPPY/ZSTD/GZIP), hive partitioning, Arrow schema
    - DuckDB setup.sql generation, Jupyter notebook template generation
    - `tests/migration/test_duckdb.cpp` - 17 tests
  - **TimescaleDB → APEX-DB** ✅ Complete
    - `include/apex/migration/timescaledb_migrator.h` + `src/migration/timescaledb_migrator.cpp`
    - Hypertable DDL, Continuous Aggregate (candlestick/vwap/ohlcv), Compression Policy
    - xbar→time_bucket, FIRST/LAST→first(col,ts), ASOF→LATERAL conversion
    - TimescaleDB Toolkit: candlestick_agg, stats_agg example generation
    - `tests/migration/test_timescaledb.cpp` - 18 tests
  - **Total tests: 70** (q→SQL 20, HDB 15, ClickHouse 18, DuckDB 17, TimescaleDB 18)
  - **Strategic backlog: Snowflake/Delta Lake Hybrid support** (4 weeks, $3.5M ARR)
    - Snowflake connector (2 weeks) - JDBC/ODBC integration, cold data queries
    - Delta Lake Reader (2 weeks) - Parquet + transaction log reading
    - Hybrid architecture guide - "Snowflake for batch, APEX-DB for real-time"
    - **Target workloads:**
      - Real-time financial analytics (20 customers × $50K = $1M)
      - IoT/sensor data (10 customers × $50K = $500K)
      - AdTech real-time bidding (10 customers × $100K = $1M)
      - Regulated on-premises industry (5 customers × $200K = $1M)
    - **Business value:** Complementary strategy, solving Snowflake customers' real-time pain
- [ ] **Bare-metal tuning detailed guide** — CPU pinning, NUMA, io_uring
- [ ] **Kubernetes operations guide** — Helm, monitoring, troubleshooting
- [ ] **Website & documentation site** — apex-db.io, docs.apex-db.io

## Medium Priority
- [ ] **Distributed query scheduler** — DistributedQueryScheduler implementation (on UCX transport)
  - PartialAggResult FlatBuffers serialization
  - scatter: fragments → UCX send → execution on each node
  - gather: UCX recv → PartialAggResult::merge()
  - Multi-node benchmark (2-node scatter/gather latency)
  - **Note:** Single-node parallelism already complete (LocalQueryScheduler, 8T = 3.48x)
- [ ] **Data/Compute node separation** — Run JOIN on separate Compute Node via RDMA remote_read, zero Data Node impact
- [ ] **CHUNKED mode activation** — Parallelize row splitting for single large partitions
- [ ] **exec_simple_select parallelization** — Currently only aggregation is parallel, add SELECT parallelism
- [ ] **DuckDB embedding (delegate complex JOINs)** — Arrow zero-copy pass-through
- [ ] **JIT SIMD emit** — Generate AVX2/512 vector IR from LLVM JIT
- [ ] **Multi-threaded drain** — Sharded drain threads
- [ ] **Ring Buffer dynamic adjustment** — Direct-to-storage path
- [ ] **HugePages tuning** — Automation
- [ ] **Resource isolation** — realtime (cores 0-3) vs analytics (cores 4-7) CPU pinning

## Storage & Format Extensions
- [x] **Parquet HDB storage** ✅ Completed (2026-03-22, devlog #012)
  - `ParquetWriter`: Partition → Apache Parquet serialization (Arrow C++ API)
  - ColumnType → Arrow DataType auto-mapping (including TIMESTAMP_NS UTC)
  - Compression selection: SNAPPY (default) / ZSTD / LZ4_RAW
  - `flush_to_file()` / `flush_to_buffer()` (for direct S3 streaming)
  - `HDBOutputFormat::PARQUET / BINARY / BOTH` integrated into FlushConfig
  - **Business value:** Direct DuckDB/Polars/Spark queries, data lake integration
- [x] **S3 HDB flush** ✅ Completed (2026-03-22, devlog #012)
  - `S3Sink`: local file / in-memory buffer → S3 PutObject
  - Partition path convention: `s3://{bucket}/{prefix}/{symbol}/{hour}.parquet`
  - MinIO compatible (`use_path_style=true`)
  - Async upload (`upload_file_async()`)
  - `delete_local_after_s3` option (storage savings)
  - **Business value:** Disaster recovery, cloud data lake, long-term retention
- [ ] **Parquet reading** — S3/local Parquet → APEX-DB queries
  - `HDBReader` Parquet support (`read_parquet()`)
  - Direct read from S3 (no local cache)
  - Parquet metadata-based partition pruning
- [ ] **Arrow Flight server** — Transmit Arrow format over network
  - Stream distributed query results as Arrow batches
  - Direct Pandas/Polars client connection
  - **Business value:** Accelerate data engineering team adoption

## Security & Enterprise
- [x] **TLS/SSL + API Key + RBAC + SSO** ✅ Completed (2026-03-22)
  - `include/apex/auth/rbac.h` — Role enum (admin/writer/reader/analyst/metrics), Permission bitmask
  - `include/apex/auth/api_key_store.h` + `src/auth/api_key_store.cpp` — SHA256-hashed key store, file-persisted, thread-safe
  - `include/apex/auth/jwt_validator.h` + `src/auth/jwt_validator.cpp` — HS256 + RS256 JWT validation, OIDC claims (sub/iss/aud/exp/apex_role/apex_symbols)
  - `include/apex/auth/auth_manager.h` + `src/auth/auth_manager.cpp` — central auth gateway, JWT > API key priority, audit logging via spdlog
  - `HttpServer` updated: `TlsConfig` struct, optional `AuthManager`, `set_pre_routing_handler` auth middleware
  - TLS: `httplib::SSLServer` (OpenSSL 3.2, cert/key PEM), compile-time `APEX_TLS_ENABLED` flag
  - `apex_auth` static library, OpenSSL detected and linked automatically
  - 37 new tests: RBAC, ApiKey CRUD, JWT HS256/RS256/expiry/claims, AuthManager middleware
  - **Business value:** Enterprise contract prerequisite; TLS + RBAC + SSO covers SOC2, EMIR, MiFID II audit requirements
- [x] **Rate Limiting** ✅ Completed (2026-03-22)
  - `include/apex/auth/rate_limiter.h` + `src/auth/rate_limiter.cpp` — token bucket, per-identity + per-IP
  - `Config`: `requests_per_minute=1000`, `burst_capacity=200`, `per_ip_rpm=10000`, `ip_burst=500`
  - Integrated into `AuthManager::check()` — returns 429 FORBIDDEN if rate-limited
  - Fine-grained per-bucket mutex; `shared_mutex` on bucket map (read-heavy path optimized)
  - 6 new tests: burst, exhaustion, per-identity independence, IP limits, refill, AuthManager integration
- [x] **Admin REST API** ✅ Completed (2026-03-22)
  - `POST /admin/keys` — create API key (returns plaintext once)
  - `GET /admin/keys` — list all API keys with metadata
  - `DELETE /admin/keys/:id` — revoke API key
  - `GET /admin/queries` — list active (running) queries
  - `DELETE /admin/queries/:id` — cancel a running query (sets CancellationToken)
  - `GET /admin/audit` — recent audit events (`?n=100`)
  - `GET /admin/version` — server version info
  - All endpoints require `ADMIN` permission; inline auth check in handler
- [x] **Query Timeout & Cancellation** ✅ Completed (2026-03-22)
  - `include/apex/auth/cancellation_token.h` — `atomic<bool>`, cancel/check
  - `include/apex/auth/query_tracker.h` + `src/auth/query_tracker.cpp` — active query registry
  - `QueryExecutor::execute(sql, CancellationToken*)` — sets thread_local token
  - Cancellation checks at each partition scan boundary in exec_simple_select, exec_agg, exec_group_agg
  - `HttpServer::run_query_with_tracking()` — registers query, wraps with `std::async` + `future.wait_for()`
  - `HttpServer::set_query_timeout_ms(ms)` — configurable timeout (0 = disabled)
  - 4 CancellationToken tests + 7 QueryTracker tests
- [x] **Secrets Management** ✅ Completed (2026-03-22)
  - `include/apex/auth/secrets_provider.h` + `src/auth/secrets_provider.cpp`
  - `EnvSecretsProvider` — reads environment variables (always available, fallback)
  - `FileSecretsProvider` — reads from filesystem (Docker/K8s secrets, `/run/secrets/`)
  - `VaultSecretsProvider` — HashiCorp Vault KV v2 HTTP API (TLS or plain)
  - `AwsSecretsProvider` — stub (SigV4 TODO; workaround: ExternalSecrets Operator → file)
  - `CompositeSecretsProvider` — priority chain: Vault > File > Env
  - `SecretsProviderFactory` — `create_composite()`, `create_with_vault()`, `create_with_aws()`
  - 10 new tests: env, file, composite chain, Vault unavailable fallback, active_backends
- [x] **Audit Log (In-memory + File)** ✅ Completed (2026-03-22)
  - `include/apex/auth/audit_buffer.h` — thread-safe ring buffer (10,000 events, configurable)
  - Every authenticated request pushes an `AuditEvent` to the buffer
  - `GET /admin/audit` exposes recent events as JSON (SOC2 / EMIR / MiFID II compliance)
  - spdlog file logging for long-term retention (7-year via log rotation)
  - 5 new tests: push/retrieve, last(N), capacity eviction, clear, AuthManager integration

## SQL Completeness
- [ ] **Subquery / CTE (WITH clause)** — `WITH daily AS (...) SELECT ...`
- [x] **CASE WHEN** ✅ Completed (2026-03-22)
  - `CASE WHEN cond THEN val [...] ELSE val END AS alias`
  - `CaseWhenExpr` / `CaseWhenBranch` AST nodes; `eval_case_when()` in executor
  - Works in SELECT list; THEN/ELSE are full arithmetic expressions
- [x] **UNION / INTERSECT / EXCEPT** ✅ Completed (2026-03-22)
  - `UNION ALL` — concatenate result sets
  - `UNION DISTINCT` — concatenate + deduplicate (std::set-based)
  - `INTERSECT` — rows present in both sides
  - `EXCEPT` — rows in left side not present in right side
  - `SelectStmt::SetOp` enum + `rhs` shared_ptr in AST; handled at top of `exec_select()`
- [ ] **NULL handling standardization** — INT64_MIN sentinel → actual NULL
- [x] **Date/time functions** ✅ Completed (2026-03-22)
  - `DATE_TRUNC('unit', col)` — floor timestamp to ns/us/ms/s/min/hour/day/week
  - `NOW()` — current nanosecond timestamp (`std::chrono::system_clock`)
  - `EPOCH_S(col)` — nanoseconds → seconds
  - `EPOCH_MS(col)` — nanoseconds → milliseconds
  - `ArithExpr::Kind::FUNC` node; `date_trunc_bucket()` helper in executor
- [x] **String functions / LIKE** ✅ Completed (2026-03-22)
  - `col LIKE 'pattern'` — DP glob matching (`%` = any substring, `_` = any char)
  - `col NOT LIKE 'pattern'` — negated
  - Applies to int64 columns via `std::to_string()` (e.g., `price LIKE '150%'`)
  - `Expr::Kind::LIKE` with `like_pattern` field; DP grid in `eval_expr()`
- [x] **SELECT arithmetic** ✅ Completed (2026-03-22)
  - `price * volume AS notional`, `(close - open) / open AS ret`
  - Arithmetic inside aggregates: `SUM(price * volume)`, `AVG(price - 15000)`
  - `ArithExpr` BINARY tree; `eval_arith()` static function in executor
- [x] **Multi-column GROUP BY** ✅ Completed (2026-03-22)
  - `GROUP BY symbol, price` — composite key via `VectorHash<std::vector<int64_t>>`
  - Works in both serial and parallel (`exec_group_agg_parallel`) paths
- [ ] **RIGHT JOIN / FULL OUTER JOIN** — SQL standard completion
- [ ] **EXPLAIN** — Query execution plan output (debugging, optimization)
- [ ] **SUBSTR / string manipulation** — for symbol name processing

## Client Ecosystem
- [ ] **JDBC/ODBC drivers** — Connect Tableau, Excel, BI tools
  - ClickHouse JDBC-compatible implementation
  - **Business value:** Enterprise BI team adoption (self-service for data analysts)
- [ ] **ClickHouse wire protocol** — Full binary protocol compatibility
  - Use existing CH client libraries (Go, Java, .NET) as-is
  - **Business value:** Zero-friction migration for ClickHouse users
- [ ] **Official Python package** — `pip install apex-db`
  - PyPI distribution, `apex.connect("localhost:8123")`
  - **Business value:** 10x developer adoption rate

## Streaming Data Integration
- [ ] **Apache Kafka consumer** — Kafka topics → APEX-DB real-time ingestion
  - librdkafka C++ client integration
  - Topic partition → APEX-DB partition auto-mapping
  - Offset management (at-least-once, exactly-once support)
  - Avro/Protobuf/JSON schema auto-decoding
  - **Implementation:** `src/feeds/kafka_consumer.cpp`, `include/apex/feeds/kafka_consumer.h`
  - **Business value:** Core enterprise data pipeline connection (Kafka is standard infrastructure)
  - **Target:** Fintech, adtech, e-commerce real-time analytics
- [ ] **Kafka Connect Sink** — Register APEX-DB as a Kafka Connect sink
  - Kafka Connect JSON connector plugin (Java or REST bridge)
  - Code-free Kafka stream → APEX-DB connection
  - **Business value:** DevOps/data engineering team self-service adoption
- [ ] **Apache Pulsar consumer** — Pulsar messages → APEX-DB
  - Pulsar C++ client integration
  - **Business value:** Organizations using Pulsar (Tencent, Yahoo, Verizon affiliates)
- [ ] **Redpanda compatibility** — Kafka API-compatible brokers (Redpanda, WarpStream)
  - Reuse Kafka consumer code (API compatible)
  - **Business value:** Automatic support for customers using Kafka replacement brokers
- [ ] **AWS Kinesis consumer** — Kinesis Data Streams → APEX-DB
  - AWS SDK C++ integration
  - Direct shard polling without KCL
  - **Business value:** AWS-native customers (fintech/adtech using Kinesis)

## Physical AI / Industry Specific
- [ ] **ROS2 plugin** — ROS2 topics → direct APEX-DB ingestion
  - `ros2 run apex_db ros_bridge --topic /lidar/scan`
  - **Business value:** Core for autonomous vehicle/robotics market entry
- [ ] **NVIDIA Isaac integration** — Isaac Sim sensor data → APEX-DB
  - **Business value:** Physical AI ecosystem adoption
- [ ] **OPC-UA connector** — Industrial standard protocol support
  - Direct connection to Siemens S7, Panasonic PLC factory equipment
  - **Business value:** Smart factory market entry
- [ ] **MQTT ingestion** — Direct IoT device connection
  - Eclipse Mosquitto, AWS IoT Core integration
  - **Business value:** IoT/edge computing market

## HA & Replication
- [ ] **WAL-based async replication** — Prevent data loss on Primary failure
  - WAL log → async transmission to Replica
  - **Business value:** Required for production HA
- [ ] **Auto failover** — Auto-promote Replica when Primary dies
  - Raft or simple heartbeat-based
  - **Business value:** Can provide 99.99% SLA
- [ ] **Snapshot backup** — Consistent full HDB snapshot
  - S3 upload automation
  - **Business value:** Disaster recovery (DR) support

## DDL & Data Management
- [ ] **CREATE TABLE / DROP TABLE** — Create tables without code
- [ ] **Retention Policy** — `ALTER TABLE SET TTL 30 DAYS`
  - Auto-delete HDB partitions older than 30 days
  - **Business value:** Automated storage cost management
- [ ] **Schema Evolution** — Add/remove columns with zero downtime
- [ ] **HDB Compaction** — Merge small partition files (improve read performance)

## Low Priority (After Phase C-3)
- [ ] **AWS Fleet API integration** — Warm Pool + Placement Group
- [ ] **DynamoDB metadata** — Partition map
- [ ] **Graph index (CSR)** — FDS fund flow tracking
- [ ] **InfluxDB migration** — InfluxQL → SQL (low strategic value)
- [ ] **Graviton (ARM) build test** — r8g instance, Highway SVE

## Completed
- [x] Phase E — End-to-End Pipeline MVP (5.52M ticks/sec)
- [x] Phase B — Highway SIMD + LLVM JIT (filter 272μs, VWAP 532μs)
- [x] Phase B v2 — BitMask filter (11x), JIT O3 (2.6x)
- [x] Phase A — HDB Tiered Storage + LZ4 (4.8 GB/s flush)
- [x] Phase D — Python Bridge (pybind11, zero-copy 522ns)
- [x] **Parallel query engine** — LocalQueryScheduler + WorkerPool (8T = 3.48x)
  - QueryScheduler abstraction (scatter/gather DI pattern)
  - ParallelScanExecutor (PARTITION/CHUNKED/SERIAL auto-selection)
  - Zero overhead for single-node (num_threads <= 1 or rows < 100K → SERIAL)
  - Benchmark: GROUP BY 1M rows 0.862ms → 0.248ms (8T)
- [x] **asof JOIN** — AsofJoinOperator (two-pointer O(n))
- [x] **Hash JOIN (inner/equi)** — HashJoinOperator
- [x] **GROUP BY aggregation** — sum/avg/min/max/count
- [x] **Window functions** — SUM/AVG/MIN/MAX/ROW_NUMBER/RANK/DENSE_RANK/LAG/LEAD OVER
- [x] **Financial functions** — VWAP, xbar, EMA, DELTA, RATIO, FIRST, LAST, Window JOIN (wj)
- [x] SQL Parser — Phase 1/2/3 complete (SELECT arithmetic, CASE WHEN, multi-GROUP BY, UNION/INTERSECT/EXCEPT, date/time functions, LIKE/NOT LIKE, IN, IS NULL, NOT, HAVING)
- [x] HTTP API — port 8123, ClickHouse compatible
- [x] Distributed Cluster Transport — UCXBackend, SharedMemBackend, PartitionRouter (2ns)
- [ ] Phase C — Distributed Memory (UCX complete, query scheduler TODO)

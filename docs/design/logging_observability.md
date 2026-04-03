# ZeptoDB: Logging & Observability Design

**Version:** 1.0
**Date:** 2026-03-25
**Status:** ✅ Implemented
**Related code:** `include/zeptodb/util/logger.h`, `include/zeptodb/common/logger.h`,
`include/zeptodb/auth/audit_buffer.h`, `src/server/http_server.cpp`

---

## 1. Overview

ZeptoDB's logging system covers all three observability axes required for production operations:

| Axis | Implementation | Purpose |
|----|------|------|
| **Logs** | Structured JSON logger + Access Log + Slow Query Log | Debugging, auditing, failure analysis |
| **Metrics** | Prometheus `/metrics` + MetricsCollector | Dashboards, alerting, capacity planning |
| **Traces** | X-Request-Id + query_id correlation | Request tracing, distributed tracing |

### Design Principles

1. **Structured output** — All logs are in JSON format. Analyze with `jq`, not `grep`
2. **Asynchronous I/O** — Logging does not affect hot path latency (spdlog async)
3. **Automatic rotation** — Prevents disk exhaustion (size-based rotating file)
4. **Level separation** — INFO in production, dynamically switch to DEBUG/TRACE for debugging
5. **Correlation ID** — End-to-end tracing via request_id ↔ query_id

---

## 2. Logger Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Code                          │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │ Storage/Core │  │  HTTP Server │  │  Auth/Security   │  │
│  │ ZEPTO_INFO() │  │ util::Logger │  │  AuditBuffer     │  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│         │                  │                    │             │
├─────────┼──────────────────┼────────────────────┼────────────┤
│         ▼                  ▼                    ▼             │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ common::    │  │ util::Logger │  │ spdlog           │   │
│  │ Logger      │  │ (Singleton)  │  │ "zepto_audit"    │   │
│  │ (Static)    │  │              │  │ (dedicated)      │   │
│  └──────┬──────┘  └──────┬───────┘  └────────┬─────────┘   │
│         │                 │                    │              │
│         ▼                 ▼                    ▼              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                    spdlog backend                        │ │
│  │  ┌─────────┐  ┌──────────────┐  ┌───────────────────┐  │ │
│  │  │ stdout  │  │ rotating     │  │ audit log file    │  │ │
│  │  │ (color) │  │ file sink    │  │ (7-year retain)   │  │ │
│  │  └─────────┘  └──────────────┘  └───────────────────┘  │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 Logger Classification

| Logger | Namespace | Output Format | Sink | Purpose |
|------|-------------|----------|------|------|
| **common::Logger** | `zeptodb::` | Text (`[timestamp] [level] [source:line] msg`) | stdout (color) | Engine internals (storage, ingestion, execution) |
| **util::Logger** | `zeptodb::util::` | Structured JSON | rotating file + stdout | HTTP server, operational events |
| **Audit Logger** | spdlog `"zepto_audit"` | Text | Dedicated file | Security auditing (SOC2/EMIR/MiFID II) |

### 2.2 Macro Mapping

| Macro | Logger | Level | Includes Source Location |
|--------|------|------|---------------|
| `ZEPTO_INFO(...)` | common::Logger | INFO | ✅ (`__FILE__:__LINE__`) |
| `ZEPTO_DEBUG(...)` | common::Logger | DEBUG | ✅ |
| `ZEPTO_WARN(...)` | common::Logger | WARN | ✅ |
| `ZEPTO_ERROR(...)` | common::Logger | ERROR | ✅ |
| `APEX_TRACE(...)` | common::Logger | TRACE | ✅ |
| `APEX_CRITICAL(...)` | common::Logger | CRITICAL | ✅ |
| `LOG_INFO(msg, comp)` | util::Logger | INFO | ❌ (component tag) |
| `LOG_WARN(msg, comp)` | util::Logger | WARN | ❌ (component tag) |
| `LOG_ERROR(msg, comp)` | util::Logger | ERROR | ❌ (component tag) |

---

## 3. Log Categories and Policies

### 3.1 Access Log (HTTP Requests)

Automatically recorded for all HTTP requests.

```json
{
  "request_id": "r0001a3",
  "method": "POST",
  "path": "/",
  "status": 200,
  "duration_us": 532,
  "request_bytes": 42,
  "response_bytes": 1024,
  "remote_addr": "10.0.1.5",
  "subject": "algo-service"
}
```

| Field | Description |
|------|------|
| `request_id` | Process-unique monotonic hex ID (`r<hex>`) |
| `method` | HTTP method |
| `path` | Request path |
| `status` | HTTP status code |
| `duration_us` | Request processing time (microseconds) |
| `request_bytes` | Request body size |
| `response_bytes` | Response body size |
| `remote_addr` | Client IP |
| `subject` | Authenticated user ID (API key name or JWT sub) |

**Log Level Policy:**

| HTTP Status | Log Level | Rationale |
|-----------|----------|------|
| 2xx, 3xx | INFO | Normal requests |
| 4xx | WARN | Client errors (invalid SQL, authentication failure, etc.) |
| 5xx | ERROR | Server errors (requires immediate investigation) |

**Component tag:** `"http"`

### 3.2 Slow Query Log

Records queries that take 100ms or longer, or that result in errors.

```json
{
  "query_id": "q_a1b2c3",
  "subject": "algo-service",
  "duration_us": 150234,
  "rows": 50000,
  "ok": true,
  "sql": "SELECT vwap(price, volume) FROM trades WHERE ..."
}
```

| Policy | Value | Configurable |
|------|-----|----------|
| Slow query threshold | 100ms | Runtime configuration planned |
| SQL max length | 200 characters (truncated) | Fixed for security |
| Error queries | Always recorded | — |

**Log level:** Error queries → WARN, slow queries → INFO
**Component tag:** `"query"`

### 3.3 Server Lifecycle Log

Recorded at server start/stop.

```json
{"event":"server_start","port":8123,"tls":false,"auth":true,"async":true}
{"event":"server_stop","port":8123}
```

**Component tag:** `"http"`

### 3.4 Engine Log (Storage/Ingestion/Execution)

Engine internal events are recorded in text format via `common::Logger`.

```
[2026-03-25 06:20:34.124668] [info] [pipeline.cpp:57] ZeptoPipeline initialization (arena=32MB)
[2026-03-25 06:20:34.124708] [info] [partition_manager.cpp:95] New partition: symbol=1, hour=0
[2026-03-25 06:20:34.130000] [warn] [arena_allocator.cpp:45] Hugepage allocation failed, fallback
```

**Key Events:**

| Component | Event | Level |
|----------|--------|------|
| Pipeline | Initialization, start, stop | INFO |
| PartitionManager | Partition creation/deletion | INFO |
| ArenaAllocator | Memory allocation, hugepage fallback | INFO/WARN |
| TickPlant | Initialization, queue overflow | INFO/WARN |
| WAL | Write start/complete, error | INFO/ERROR |
| HDB | Flush start/complete, compression | INFO |
| JIT | Compilation start/complete | INFO |
| FlushManager | Tiering migration (Hot→Warm→Cold) | INFO |

### 3.5 Audit Log (Security Auditing)

Records authentication/authorization events. Requires 7-year retention
for regulatory compliance (SOC2, EMIR, MiFID II).

```json
{
  "timestamp_ns": 1711234567000000000,
  "subject": "algo-service",
  "role": "writer",
  "action": "POST /",
  "detail": "apikey-auth",
  "remote_addr": "10.0.1.5"
}
```

**Dual recording:**
1. **File** — spdlog `"zepto_audit"` dedicated logger → disk file (long-term retention)
2. **Memory** — `AuditBuffer` ring buffer (10,000 entries) → `GET /admin/audit` API

**Recorded events:**

| Event | Recorded |
|--------|----------|
| Authentication success | ✅ |
| Authentication failure (401) | ✅ |
| Authorization failure (403) | ✅ |
| Rate limit exceeded (429) | ✅ |
| API key creation/deletion | ✅ |
| Query kill (admin) | ✅ |

---

## 4. Log Rotation and Retention Policies

### 4.1 File Rotation

| Log Type | File Path | Max Size | Max File Count | Retention Period |
|-----------|----------|----------|-------------|----------|
| Application (JSON) | `/var/log/zeptodb/zeptodb.log` | 100 MB | 10 files | ~1 GB total |
| Audit | `/var/log/zeptodb/audit.log` | Configurable | External rotation | 7 years (regulatory) |
| Engine (stdout) | systemd journal or redirect | — | — | Infrastructure policy |

### 4.2 Recommended Settings by Environment

| Environment | Log Level | Access Log | Slow Query | Audit |
|------|----------|------------|------------|-------|
| **Development** | DEBUG | ✅ | ✅ (>0ms) | ❌ |
| **Staging** | INFO | ✅ | ✅ (>50ms) | ✅ |
| **Production** | INFO | ✅ | ✅ (>100ms) | ✅ |
| **HFT (ultra-low latency)** | WARN | ❌ (Prometheus only) | ✅ (>1ms) | ✅ |

### 4.3 Disk Usage Estimates

| Scenario | QPS | Access Log/day | Slow Query/day | Total Disk/day |
|----------|-----|--------------|--------------|-------------|
| Small | 100 | ~50 MB | ~1 MB | ~51 MB |
| Medium | 1,000 | ~500 MB | ~10 MB | ~510 MB |
| Large | 10,000 | ~5 GB | ~100 MB | ~5.1 GB |
| HFT | 100,000 | Access Log OFF | ~1 GB | ~1 GB |

For large-scale environments, it is recommended to sample access logs or stream them
to an external log collector (Fluentd/Vector).

---

## 5. Tracing (Request Correlation)

### 5.1 Request ID Chain

```
Client                    HttpServer                    QueryExecutor
  │                          │                              │
  │  POST / (SQL)            │                              │
  │ ─────────────────────►   │                              │
  │                          │  X-Zepto-Request-Id: r00a1   │
  │                          │  X-Zepto-Start-Us: 170000    │
  │                          │  X-Zepto-Subject: algo-svc   │
  │                          │                              │
  │                          │  query_id = q_xxxx           │
  │                          │ ────────────────────────►    │
  │                          │                              │
  │                          │  Access Log (request_id)     │
  │                          │  Slow Query Log (query_id)   │
  │                          │                              │
  │  ◄─────────────────────  │                              │
  │  X-Request-Id: r00a1     │                              │
```

### 5.2 Correlation Tracing Methods

**Scenario: Client reports a slow response**

```bash
# 1. Search access log using the X-Request-Id received by the client
jq 'select(.request_id == "r00a1")' /var/log/zeptodb/zeptodb.log

# 2. Check slow query log for the same time range
jq 'select(.component == "query" and .duration_us > 100000)' /var/log/zeptodb/zeptodb.log

# 3. Trace all requests for a specific subject
jq 'select(.subject == "algo-service")' /var/log/zeptodb/zeptodb.log

# 4. Filter only 4xx/5xx errors
jq 'select(.status >= 400)' /var/log/zeptodb/zeptodb.log

# 5. Calculate P99 latency for a specific time range
jq -s '[.[] | select(.component == "http") | .duration_us] | sort | .[(length * 0.99 | floor)]' \
  /var/log/zeptodb/zeptodb.log
```

### 5.3 Distributed Environment Tracing

In cluster mode, when the coordinator scatters to remote nodes:

```
Client → Coordinator (request_id: r00a1)
           ├── Node 1 (METRICS_REQUEST, logged locally)
           ├── Node 2 (METRICS_REQUEST, logged locally)
           └── Node 3 (METRICS_REQUEST, logged locally)
```

Each node records its own access log. Through centralized log collection (Fluentd → S3/ES),
the entire path can be traced using the `request_id`.

---

## 6. Prometheus Metrics (Real-Time Monitoring)

Separately from logs, Prometheus metrics are provided for real-time monitoring.

### 6.1 HTTP Metrics

| Metric | Type | Description |
|--------|------|------|
| `zepto_http_requests_total` | counter | Total HTTP request count |
| `zepto_http_active_sessions` | gauge | Current active session count |

### 6.2 Engine Metrics

| Metric | Type | Description |
|--------|------|------|
| `zepto_ticks_ingested_total` | counter | Total ingested tick count |
| `zepto_ticks_stored_total` | counter | Total stored tick count |
| `zepto_ticks_dropped_total` | counter | Dropped tick count |
| `zepto_queries_executed_total` | counter | Total query execution count |
| `zepto_rows_scanned_total` | counter | Total scanned row count |
| `zepto_server_up` | gauge | Server up status |
| `zepto_server_ready` | gauge | Server ready status |

### 6.3 MetricsCollector (Time-Series History)

`MetricsCollector` snapshots `PipelineStats` at 3-second intervals and stores them
in an in-memory ring buffer. Queryable via `GET /admin/metrics/history`.

| Setting | Default | Description |
|------|--------|------|
| `interval` | 3 seconds | Capture interval |
| `capacity` | 1,200 | Maximum snapshot count (1 hour) |
| `max_memory_bytes` | 256 KB | Memory hard limit |
| `response_limit` | 600 | Maximum API response count (30 minutes) |

---

## 7. Operations Guide

### 7.1 Logger Initialization

```cpp
// At server startup (main or server initialization code)
#include "zeptodb/util/logger.h"

// Initialize structured JSON logger
zeptodb::util::Logger::instance().init(
    "/var/log/zeptodb",    // Log directory
    zeptodb::util::LogLevel::INFO,  // Level
    100,                   // Max 100 MB per file
    10                     // Max 10 files (1 GB total)
);
```

### 7.2 Runtime Level Change

```cpp
// Lower level for debugging
zeptodb::util::Logger::instance().set_level(zeptodb::util::LogLevel::DEBUG);

// Restore after recovery
zeptodb::util::Logger::instance().set_level(zeptodb::util::LogLevel::INFO);
```

### 7.3 Log Collection Pipeline (Recommended)

```
ZeptoDB Node                    Log Collector              Storage
┌──────────┐                   ┌──────────┐              ┌─────────┐
│ zeptodb  │  rotating file    │ Fluentd  │   S3/ES/     │ S3      │
│ .log     │ ───────────────►  │ /Vector  │ ──────────►  │ OpenES  │
│          │                   │          │              │ Loki    │
└──────────┘                   └──────────┘              └─────────┘
│ audit    │  dedicated file   │ Fluentd  │   S3 (7yr)   │ S3      │
│ .log     │ ───────────────►  │ /Vector  │ ──────────►  │ Glacier │
└──────────┘                   └──────────┘              └─────────┘
│ /metrics │  Prometheus pull  │ Prom     │              │ Prom    │
│          │ ◄───────────────  │ Server   │ ──────────►  │ Grafana │
└──────────┘                   └──────────┘              └─────────┘
```

### 7.4 Grafana Alert Rules (Log-Based)

| Alert | Condition | Severity |
|------|------|--------|
| High error rate | 5xx > 1% (5-minute window) | Critical |
| Slow query spike | P99 > 500ms (5-minute window) | Warning |
| Auth failure burst | 401/403 > 100/min | Warning |
| Disk usage | Log directory > 80% | Warning |

---

## 8. Security Considerations

| Item | Policy |
|------|------|
| SQL logging | 200-character truncation (prevents sensitive data exposure) |
| API Key | Never recorded in logs (only subject name is recorded) |
| PII | Business data such as prices/volumes not included in logs |
| Audit log access | Only ADMIN role can access `GET /admin/audit` |
| Log file permissions | `0640` (owner: zeptodb, group: ops) |
| Transport encryption | TLS required when transmitting from log collector → S3 |

---

## 9. File Map

```
include/zeptodb/
├── util/logger.h              # Structured JSON logger (singleton)
├── common/logger.h            # Engine internal text logger (static)
├── auth/audit_buffer.h        # Audit event ring buffer
└── server/
    ├── http_server.h          # Access log, X-Request-Id documentation
    └── metrics_collector.h    # Time-series metrics collector

src/
├── util/logger.cpp            # JSON logger implementation (async spdlog)
├── common/logger.cpp          # Text logger implementation
├── auth/auth_manager.cpp      # Audit logger ("zepto_audit")
└── server/http_server.cpp     # Access log, slow query log implementation

docs/
├── design/logging_observability.md  # This document
└── devlog/029_http_observability.md # Implementation record
```

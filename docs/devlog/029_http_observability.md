# 029 — HTTP Server Observability: Structured Access Logging & Request Tracing

> Date: 2026-03-25
> Status: Complete — 10 tests PASS

---

## Problem

The HTTP server had zero request-level logging. No way to trace individual
requests, identify slow queries, or correlate client-side errors with
server-side events in production.

## Solution

Added production-grade observability to `HttpServer`:

### 1. Structured JSON Access Log (every request)

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

- Emitted via `zeptodb::util::Logger` (async JSON, rotating file)
- Log level: INFO (2xx/3xx), WARN (4xx), ERROR (5xx)
- Component tag: `"http"`

### 2. Slow Query Log (>100ms or error)

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

- SQL truncated to 200 chars for log safety
- Component tag: `"query"`

### 3. X-Request-Id Response Header

Every HTTP response includes `X-Request-Id: r<hex>` for client-side
correlation. Monotonic counter ensures uniqueness within a process.

### 4. Server Lifecycle Events

```json
{"event":"server_start","port":8123,"tls":false,"auth":true,"async":true}
{"event":"server_stop","port":8123}
```

### 5. Prometheus Metrics

- `zepto_http_requests_total` — total HTTP requests served (counter)
- `zepto_http_active_sessions` — current active sessions (gauge)

## Changes

| File | Change |
|------|--------|
| `src/server/http_server.cpp` | Access log, slow query log, request ID, lifecycle events |
| `include/zeptodb/server/http_server.h` | Updated endpoint/observability docs |
| `CMakeLists.txt` | Added `src/util/logger.cpp` to `zepto_common` |

## Tests

| Test | Verifies |
|------|----------|
| `ConnectionHooksTest.ResponseContainsRequestId` | X-Request-Id in response |
| `ConnectionHooksTest.WhoamiReturnsAdminWhenNoAuth` | /whoami endpoint |
| All existing ConnectionHooks + MetricsProvider tests | No regression |

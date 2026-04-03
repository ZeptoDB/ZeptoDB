# ZeptoDB Web UI — Architecture & API Reference

## Overview

Web Console based on Next.js 16. Connects to the ZeptoDB HTTP server (port 8123) to provide SQL execution, dashboard monitoring, and table exploration.

```
Browser (:3000)  →  Next.js rewrites (/api/*)  →  ZeptoDB HTTP Server (:8123)
```

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Framework | Next.js 16.2.1 (App Router, React Compiler) |
| UI | MUI 7, Emotion |
| SQL Editor | CodeMirror (@uiw/react-codemirror, @codemirror/lang-sql) |
| Charts | Recharts |
| Data Fetching | TanStack React Query v5 |
| Package Manager | pnpm |

## Directory Structure

```
web/
├── src/
│   ├── app/
│   │   ├── layout.tsx          # Root layout (metadata, fonts, ClientShell)
│   │   ├── page.tsx            # / → redirect to /query
│   │   ├── globals.css         # Global reset
│   │   ├── login/page.tsx      # Login (API Key / JWT tabs + Dev Mode)
│   │   ├── query/page.tsx      # SQL Editor + result table + query history
│   │   ├── dashboard/page.tsx  # Stats cards + ingestion rate chart
│   │   ├── tables/page.tsx     # SHOW TABLES + DESCRIBE + preview
│   │   └── cluster/page.tsx    # Cluster node status + per-node metrics history
│   ├── components/
│   │   ├── ClientShell.tsx     # Auth + layout wrapper (login bypass)
│   │   ├── AuthGuard.tsx       # Redirect to /login if not authenticated
│   │   ├── Providers.tsx       # QueryClient + MUI ThemeProvider
│   │   ├── Sidebar.tsx         # Left nav (role-based: Query, Dashboard, Tables, Cluster)
│   │   └── TopBar.tsx          # Health status chip + role badge
│   │   ├── ClientShell.tsx     # Auth + layout wrapper (login bypass)
│   │   ├── AuthGuard.tsx       # Redirect to /login if not authenticated
│   │   ├── Providers.tsx       # QueryClient + MUI ThemeProvider
│   │   ├── Sidebar.tsx         # Left nav (Query, Dashboard, Tables, Logout)
│   │   └── TopBar.tsx          # Health status chip + role badge
│   ├── lib/
│   │   ├── api.ts              # API client (querySQL, fetchStats, fetchHealth)
│   │   └── auth.tsx            # AuthContext (sessionStorage, login/logout)
│   └── theme/
│       └── theme.ts            # MUI dark theme (Indigo + Amber)
├── next.config.ts              # Rewrites: /api/* → localhost:8123/*
├── package.json
└── tsconfig.json
```

## API Proxy (next.config.ts)

The Web UI does not send requests directly to port 8123; instead, it proxies them through Next.js rewrites:

```
/api      → http://localhost:8123/
/api/     → http://localhost:8123/
/api/:path* → http://localhost:8123/:path*
```

Therefore, a `fetch("/api", ...)` call from the Web UI is actually forwarded to `http://localhost:8123/`.

## Authentication Flow

### Login Page (`/login`)

1. API Key tab: Validates the key by executing `POST /api` (body: `SELECT 1`) with a `Bearer <key>` header
2. JWT tab: Same approach (SSO is Coming Soon)
3. Dev Mode: Enters without authentication using the `__dev__` token (apiKey is an empty string)

### Auth State

- Stored in `sessionStorage` under the `zepto_auth` key as `{ apiKey, role, subject }`
- `AuthGuard` redirects to `/login` if not authenticated
- All API calls include the `Authorization: Bearer <apiKey>` header

### Role Detection (during login)

```
POST /api (SELECT 1) → If successful, the key is valid
GET /api/admin/keys  → 200 means admin, 403 means writer, otherwise reader
```

## Web UI → Server API Mapping

| Web UI Call | Server Endpoint | Auth Required | Purpose |
|-------------|----------------|----------|------|
| `POST /api` (body: SQL) | `POST /` | ✅ Yes | Execute SQL query |
| `GET /api/stats` | `GET /stats` | ✅ Yes | Dashboard statistics |
| `GET /api/health` | `GET /health` | ❌ No (public) | TopBar connection status |
| `GET /api/admin/keys` | `GET /admin/keys` | ✅ Admin only | Role detection (login) |
| `GET /api/admin/nodes` | `GET /admin/nodes` | ✅ Admin only | Cluster node list |
| `GET /api/admin/cluster` | `GET /admin/cluster` | ✅ Admin only | Cluster overview |
| `GET /api/admin/metrics/history` | `GET /admin/metrics/history` | ✅ Admin only | Metrics time-series history (3-second interval, 1-hour buffer) |

## Server Endpoints (http_server.cpp)

### Public (no authentication required)

| Method | Path | Description |
|--------|------|------|
| GET | `/ping` | ClickHouse-compatible health check ("Ok") |
| GET | `/health` | K8s liveness probe |
| GET | `/ready` | K8s readiness probe |

### Authenticated (Bearer token required)

| Method | Path | Description |
|--------|------|------|
| POST | `/` | Execute SQL (body = SQL string) |
| GET | `/` | Execute SQL (query param: `?query=SELECT...`) |
| GET | `/stats` | Pipeline statistics (ticks_ingested, ticks_stored, etc.) |
| GET | `/metrics` | Prometheus OpenMetrics |

### Admin Only (ADMIN role required)

| Method | Path | Description |
|--------|------|------|
| POST | `/admin/keys` | Create API Key |
| GET | `/admin/keys` | List API Keys |
| DELETE | `/admin/keys/:id` | Revoke API Key |
| GET | `/admin/queries` | List running queries |
| DELETE | `/admin/queries/:id` | Cancel query |
| GET | `/admin/audit` | Audit log (most recent N entries) |
| GET | `/admin/sessions` | List active sessions |
| GET | `/admin/version` | Server version |
| GET | `/admin/nodes` | Cluster node information |
| GET | `/admin/cluster` | Cluster overview |

## Known Issue: "Invalid API" Error

### Cause (fixed)

There were two issues:

1. **Dev Mode empty token**: Logging in with `__dev__` set `apiKey: ""`, which sent `Bearer ` (empty token) → 401
2. **SELECT 1 parser error**: Key validation during login used `SELECT 1`, but the ZeptoDB parser did not support `SELECT 1` (FROM clause required) → parser error → misidentified as "Invalid API key"

### Fix Details

- `auth.tsx`: Removed Dev Mode, changed key validation to `GET /stats` (no SQL parsing required)
- `api.ts`: Added empty string apiKey check (`apiKey?.length`), improved error handling
- `login/page.tsx`: Removed Dev Mode button, added server console instructions
- `tables/page.tsx`: Replaced `SHOW TABLES`/`DESCRIBE` with `/admin/cluster` + actual queries

## Quick Start

```bash
# 1. Start the ZeptoDB server (from the build directory)
./tools/zepto_http_server --port 8123 --ticks 10000

# 2. Start the Web UI
cd web
pnpm install
pnpm dev

# 3. Open http://localhost:3000 in your browser
# 4. Log in with the admin API key printed in the server console
```

## Response Format

JSON format of SQL results returned by the server:

```json
{
  "columns": ["symbol", "price", "volume"],
  "data": [[1, 15000, 100], [1, 15001, 101]],
  "rows": 2,
  "execution_time_us": 272
}
```

Error response:

```json
{
  "error": "Error message here"
}
```

## SQL Commands Used by Web UI

| Page | SQL / Endpoint |
|------|----------------|
| Tables (list) | `SHOW TABLES` |
| Tables (schema) | `DESCRIBE <table_name>` |
| Tables (preview) | `SELECT * FROM <table> LIMIT 20` |
| Query | User-entered SQL |
| Query (history) | localStorage (`zepto_query_history`, max 50) |
| Dashboard | `GET /stats` (7 fields: ticks_ingested, ticks_stored, ticks_dropped, queries_executed, total_rows_scanned, partitions_created, last_ingest_latency_ns) |

## Role-Based Menu Filtering

The sidebar menu is filtered based on the logged-in user's role:

| Role | Query | Dashboard | Tables | Cluster |
|------|:-----:|:---------:|:------:|:-------:|
| admin | ✅ | ✅ | ✅ | ✅ |
| writer | ✅ | ✅ | ✅ | ❌ |
| reader | ✅ | ✅ | ✅ | ❌ |
| analyst | ✅ | ❌ | ✅ | ❌ |
| metrics | ❌ | ✅ | ❌ | ❌ |

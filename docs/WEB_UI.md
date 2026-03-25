# ZeptoDB Web UI — Architecture & API Reference

## Overview

Next.js 16 기반 Web Console. ZeptoDB HTTP 서버(port 8123)에 연결하여 SQL 실행, 대시보드 모니터링, 테이블 탐색을 제공한다.

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

Web UI는 직접 8123에 요청하지 않고, Next.js rewrite를 통해 프록시한다:

```
/api      → http://localhost:8123/
/api/     → http://localhost:8123/
/api/:path* → http://localhost:8123/:path*
```

따라서 Web UI의 `fetch("/api", ...)` 호출은 실제로 `http://localhost:8123/`로 전달된다.

## Authentication Flow

### Login Page (`/login`)

1. API Key 탭: `Bearer <key>` 헤더로 `POST /api` (body: `SELECT 1`) 실행하여 키 유효성 검증
2. JWT 탭: 동일 방식 (SSO는 Coming Soon)
3. Dev Mode: `__dev__` 토큰으로 인증 없이 진입 (apiKey 빈 문자열)

### Auth State

- `sessionStorage`에 `zepto_auth` 키로 `{ apiKey, role, subject }` 저장
- `AuthGuard`가 미인증 시 `/login`으로 redirect
- 모든 API 호출에 `Authorization: Bearer <apiKey>` 헤더 첨부

### Role Detection (login 시)

```
POST /api (SELECT 1) → 성공하면 유효한 키
GET /api/admin/keys  → 200이면 admin, 403이면 writer, 실패하면 reader
```

## Web UI → Server API Mapping

| Web UI 호출 | 서버 엔드포인트 | 인증 필요 | 용도 |
|-------------|----------------|----------|------|
| `POST /api` (body: SQL) | `POST /` | ✅ Yes | SQL 쿼리 실행 |
| `GET /api/stats` | `GET /stats` | ✅ Yes | Dashboard 통계 |
| `GET /api/health` | `GET /health` | ❌ No (public) | TopBar 연결 상태 |
| `GET /api/admin/keys` | `GET /admin/keys` | ✅ Admin only | Role detection (login) |
| `GET /api/admin/nodes` | `GET /admin/nodes` | ✅ Admin only | Cluster 노드 목록 |
| `GET /api/admin/cluster` | `GET /admin/cluster` | ✅ Admin only | Cluster 개요 |
| `GET /api/admin/metrics/history` | `GET /admin/metrics/history` | ✅ Admin only | Metrics 시계열 히스토리 (3초 간격, 1시간 버퍼) |

## Server Endpoints (http_server.cpp)

### Public (인증 불필요)

| Method | Path | 설명 |
|--------|------|------|
| GET | `/ping` | ClickHouse 호환 health check ("Ok") |
| GET | `/health` | K8s liveness probe |
| GET | `/ready` | K8s readiness probe |

### Authenticated (Bearer token 필요)

| Method | Path | 설명 |
|--------|------|------|
| POST | `/` | SQL 실행 (body = SQL string) |
| GET | `/` | SQL 실행 (query param: `?query=SELECT...`) |
| GET | `/stats` | Pipeline 통계 (ticks_ingested, ticks_stored 등) |
| GET | `/metrics` | Prometheus OpenMetrics |

### Admin Only (ADMIN role 필요)

| Method | Path | 설명 |
|--------|------|------|
| POST | `/admin/keys` | API Key 생성 |
| GET | `/admin/keys` | API Key 목록 |
| DELETE | `/admin/keys/:id` | API Key 폐기 |
| GET | `/admin/queries` | 실행 중인 쿼리 목록 |
| DELETE | `/admin/queries/:id` | 쿼리 취소 |
| GET | `/admin/audit` | 감사 로그 (최근 N건) |
| GET | `/admin/sessions` | 활성 세션 목록 |
| GET | `/admin/version` | 서버 버전 |
| GET | `/admin/nodes` | 클러스터 노드 정보 |
| GET | `/admin/cluster` | 클러스터 개요 |

## Known Issue: "Invalid API" Error

### 원인 (수정 완료)

두 가지 문제가 있었다:

1. **Dev Mode 빈 토큰**: `__dev__`로 로그인하면 `apiKey: ""`가 설정되어 `Bearer ` (빈 토큰)이 전송됨 → 401
2. **SELECT 1 파서 에러**: 로그인 시 `SELECT 1`로 키 검증했으나, ZeptoDB 파서가 `SELECT 1` 미지원 (FROM 필요) → 파서 에러 → "Invalid API key"로 오인

### 수정 내용

- `auth.tsx`: Dev Mode 제거, 키 검증을 `GET /stats`로 변경 (SQL 파싱 불필요)
- `api.ts`: 빈 문자열 apiKey 체크 (`apiKey?.length`), 에러 핸들링 강화
- `login/page.tsx`: Dev Mode 버튼 제거, 서버 콘솔 안내 추가
- `tables/page.tsx`: `SHOW TABLES`/`DESCRIBE` 대신 `/admin/cluster` + 실제 쿼리 사용

## Quick Start

```bash
# 1. ZeptoDB 서버 시작 (build 디렉토리에서)
./tools/zepto_http_server --port 8123 --ticks 10000

# 2. Web UI 시작
cd web
pnpm install
pnpm dev

# 3. 브라우저에서 http://localhost:3000 접속
# 4. 서버 콘솔에 출력된 admin API key로 로그인
```

## Response Format

서버가 반환하는 SQL 결과 JSON 형식:

```json
{
  "columns": ["symbol", "price", "volume"],
  "data": [[1, 15000, 100], [1, 15001, 101]],
  "rows": 2,
  "execution_time_us": 272
}
```

에러 응답:

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
| Query | 사용자 입력 SQL |
| Query (history) | localStorage (`zepto_query_history`, max 50) |
| Dashboard | `GET /stats` (7 fields: ticks_ingested, ticks_stored, ticks_dropped, queries_executed, total_rows_scanned, partitions_created, last_ingest_latency_ns) |

## Role-Based Menu Filtering

Sidebar 메뉴는 로그인한 사용자의 role에 따라 필터링된다:

| Role | Query | Dashboard | Tables | Cluster |
|------|:-----:|:---------:|:------:|:-------:|
| admin | ✅ | ✅ | ✅ | ✅ |
| writer | ✅ | ✅ | ✅ | ❌ |
| reader | ✅ | ✅ | ✅ | ❌ |
| analyst | ✅ | ❌ | ✅ | ❌ |
| metrics | ❌ | ✅ | ❌ | ❌ |

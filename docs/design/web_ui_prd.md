# ZeptoDB Web UI — PRD

## 1. Overview

Web UI for investor/customer demos. Next.js project in the `web/` directory within the `zeptodb` repo.
`zepto_server` serves the built static files at `/ui/*`.

## 2. Tech Stack

| Layer | Choice | Rationale |
|-------|--------|-----------|
| Framework | Next.js 15 (App Router) | File-based routing, SSG, extensibility |
| UI | **MUI (Material UI) v6** | Material Design 3, rich dashboard components, built-in dark mode |
| SQL Editor | CodeMirror 6 | SQL highlighting, autocomplete |
| Chart | Recharts | React native, lightweight |
| State | TanStack Query | API polling/caching |
| Package | pnpm | Fast and efficient |

## 2.1 Design Direction

**Tone**: Polished startup — clean and minimal, data-centric

**Color Palette**:
- Primary: Indigo (`#3F51B5`) — trustworthy, technical
- Secondary: Amber (`#FFC107`) — accent, CTA
- Background: `#FAFAFA` (light) / `#121212` (dark)
- Surface: `#FFFFFF` (light) / `#1E1E1E` (dark)
- Dark mode by default (database tool = developer → prefers dark)

**Typography**:
- Header: Inter (or MUI default Roboto)
- Code/SQL: JetBrains Mono

**Component Style**:
- MUI `<DataGrid>` — result table (built-in sorting, filtering, pagination)
- MUI `<Card>` — dashboard metric cards
- MUI `<Drawer>` — sidebar navigation
- MUI `<AppBar>` — top bar (server status indicator, dark mode toggle)
- MUI `<Chip>` — role/status badges
- MUI `<Snackbar>` — query success/failure notifications
- MUI `<Dialog>` — key creation, tenant creation modals

**Layout**:
```
┌─────────────────────────────────────────────────┐
│  AppBar: ZeptoDB logo │ server status │ 🌙 │ 👤 │
├────────┬────────────────────────────────────────┤
│        │                                        │
│  Nav   │  Main Content                          │
│  ----  │                                        │
│  Query │  ┌─────────────────────────────────┐   │
│  Tables│  │  SQL Editor (CodeMirror)        │   │
│  Dash  │  ├─────────────────────────────────┤   │
│  ----  │  │  Result DataGrid                │   │
│  Keys  │  │  272μs · 1,000 rows scanned     │   │
│  Roles │  └─────────────────────────────────┘   │
│  Tenant│                                        │
│  Audit │                                        │
│  ----  │                                        │
│  ⚙️    │                                        │
├────────┴────────────────────────────────────────┤
│  Footer: v0.1.0 │ GitHub │ Docs                 │
└─────────────────────────────────────────────────┘
```

## 3. Page Structure

```
web/src/app/
├── layout.tsx                    # Sidebar + Header
├── page.tsx                      # / → /query redirect
│
├── (console)/                    # ── Console Area ──
│   ├── layout.tsx                # Sidebar shared layout
│   │
│   │  # ── P1: MVP ──
│   ├── query/page.tsx            # SQL editor + result table
│   ├── dashboard/page.tsx        # Cluster status dashboard
│   ├── tables/
│   │   ├── page.tsx              # Table list
│   │   └── [name]/page.tsx       # Table detail (schema, sample)
│   │
│   │  # ── P2: Governance ──
│   ├── keys/
│   │   ├── page.tsx              # API key list + create/revoke
│   │   └── [id]/page.tsx         # Key detail (role, symbols, usage history)
│   ├── roles/page.tsx            # Role matrix (5 roles × permissions)
│   ├── tenants/
│   │   ├── page.tsx              # Tenant list + create/delete
│   │   └── [id]/page.tsx         # Tenant detail (quota, usage)
│   ├── audit/page.tsx            # Audit log viewer (filter/search)
│   ├── sessions/page.tsx         # Active session list
│   ├── queries/page.tsx          # Running queries + kill
│   │
│   │  # ── P3: Settings ──
│   └── settings/page.tsx         # Server settings, TLS, rate limit
│
└── (marketing)/                  # ── Future Product Website ──
    ├── layout.tsx
    ├── home/page.tsx
    ├── features/page.tsx
    └── pricing/page.tsx
```

## 4. Phase Details

### P1: MVP — "A Product We Can Demo" (Day 1-5)

#### Query Editor (`/query`)
- CodeMirror SQL editor (Cmd+Enter to execute)
- Result table (sorting, pagination)
- Display execution time + rows scanned
- Query history (localStorage)
- API: `POST /`

#### Dashboard (`/dashboard`)
- Server status (health, uptime)
- Real-time counters: ticks ingested/stored/dropped, queries executed
- Ingestion rate time-series chart
- API: `GET /stats` (2-second polling)

#### Tables (`/tables`)
- Table list + row count, column count
- Click table → schema detail (column names, types)
- Sample data preview (LIMIT 20)
- API: `POST /` → `SHOW TABLES`, `DESCRIBE <table>`

### P2: Governance — Accounts/Permissions/Audit (Day 6-10)

#### API Key Management (`/keys`)
- Key list: id, name, role, enabled, created_at, last_used
- Key creation: select name + role + enter allowed_symbols
- Key revocation: revoke button (soft delete)
- Key detail: usage history, list of accessed symbols
- API: `GET/POST/DELETE /admin/keys`

#### Role Matrix (`/roles`)
- 5 roles × 4 permissions matrix (read-only view)

| Role | READ | WRITE | ADMIN | METRICS |
|------|------|-------|-------|---------|
| admin | ✅ | ✅ | ✅ | ✅ |
| writer | ✅ | ✅ | ❌ | ✅ |
| reader | ✅ | ❌ | ❌ | ❌ |
| analyst | ✅ (symbol restricted) | ❌ | ❌ | ❌ |
| metrics | ❌ | ❌ | ❌ | ✅ |

- Description and usage scenarios per role
- Link to list of keys for each role

#### Tenant Management (`/tenants`)
- Tenant list: id, name, priority, namespace
- Tenant creation: quota settings (concurrent queries, memory, rate, ingestion)
- Tenant detail:
  - Resource quota vs actual usage gauge
  - active_queries / total_queries / rejected_queries
  - Table namespace scope
- API: Backend TenantManager (REST endpoint needs to be added)

#### Audit Log (`/audit`)
- Recent audit events table (timestamp, subject, role, action, detail, IP)
- Filters: subject, role, action, time range
- Real-time streaming (polling)
- CSV export
- API: `GET /admin/audit`

#### Active Sessions (`/sessions`)
- List of connected clients (IP, user, connected_at, last_active, query_count)
- Evict idle session button
- API: `GET /admin/sessions`

#### Running Queries (`/queries`)
- List of running queries (id, subject, SQL, elapsed)
- Kill button (CancellationToken)
- API: `GET /admin/queries`, `DELETE /admin/queries/:id`

### P3: Settings + Marketing (Day 11+)

- Server settings view (TLS, rate limit, timeout)
- Marketing pages (landing, features, pricing)

## 5. Backend Changes Required

| Item | Description | Priority |
|------|-------------|----------|
| `GET /ui/*` | Static file serving (Next.js export) | P1 |
| `SHOW TABLES` | Table list SQL | P1 |
| `DESCRIBE <table>` | Column schema SQL | P1 |
| CORS | localhost:3000 → :8123 during development | P1 |
| `GET/POST/DELETE /admin/tenants` | Tenant CRUD REST API | P2 |
| `GET /admin/sessions` | Session list JSON | P2 |
| `GET /admin/keys/:id/usage` | Per-key usage history | P2 |

## 6. Auth Flow in Web UI

```
Browser → /ui (login page)
  ↓ Enter API Key (or JWT SSO)
  ↓ POST /admin/keys/validate → returns AuthContext
  ↓ Filter sidebar menu based on role:
      admin   → all menus
      writer  → query, tables, dashboard
      reader  → query, tables, dashboard (SELECT only)
      analyst → query (symbol restricted), tables
      metrics → dashboard only
```

## 7. Dev Workflow

```bash
# Development (hot reload)
cd web && pnpm dev              # localhost:3000
# Proxy to localhost:8123 via next.config.js rewrites

# Production build
pnpm build                      # web/out/ static files
# zepto_server serves at GET /ui/*
```

## 8. Success Criteria

- [ ] Able to demo SQL execution in investor meetings
- [ ] View cluster status on a single screen
- [ ] Create/revoke API keys from the UI
- [ ] Search/filter audit logs from the UI
- [ ] Role-based menu access control works

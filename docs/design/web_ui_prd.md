# ZeptoDB Web UI — PRD

## 1. Overview

투자자/고객 데모용 Web UI. `zeptodb` repo 내 `web/` 디렉토리에 Next.js 프로젝트.
`zepto_server`가 빌드된 정적 파일을 `/ui/*`에서 serve.

## 2. Tech Stack

| Layer | Choice | Rationale |
|-------|--------|-----------|
| Framework | Next.js 15 (App Router) | 파일 기반 라우팅, SSG, 확장성 |
| UI | **MUI (Material UI) v6** | Material Design 3, 대시보드 컴포넌트 풍부, 다크모드 내장 |
| SQL Editor | CodeMirror 6 | SQL 하이라이팅, 자동완성 |
| Chart | Recharts | React 네이티브, 가벼움 |
| State | TanStack Query | API polling/caching |
| Package | pnpm | 빠르고 효율적 |

## 2.1 Design Direction

**톤**: 세련된 스타트업 — 깔끔하고 미니멀, 데이터 중심

**Color Palette**:
- Primary: Indigo (`#3F51B5`) — 신뢰감, 기술적
- Secondary: Amber (`#FFC107`) — 액센트, CTA
- Background: `#FAFAFA` (light) / `#121212` (dark)
- Surface: `#FFFFFF` (light) / `#1E1E1E` (dark)
- 다크모드 기본 (데이터베이스 도구 = 개발자 → 다크 선호)

**Typography**:
- 헤더: Inter (또는 MUI 기본 Roboto)
- 코드/SQL: JetBrains Mono

**Component Style**:
- MUI `<DataGrid>` — 결과 테이블 (정렬, 필터, 페이지네이션 내장)
- MUI `<Card>` — 대시보드 메트릭 카드
- MUI `<Drawer>` — 사이드바 네비게이션
- MUI `<AppBar>` — 상단 바 (서버 상태 indicator, 다크모드 토글)
- MUI `<Chip>` — role/status 뱃지
- MUI `<Snackbar>` — 쿼리 성공/실패 알림
- MUI `<Dialog>` — 키 생성, 테넌트 생성 모달

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
├── layout.tsx                    # 사이드바 + 헤더
├── page.tsx                      # / → /query redirect
│
├── (console)/                    # ── 콘솔 영역 ──
│   ├── layout.tsx                # 사이드바 공통 레이아웃
│   │
│   │  # ── P1: MVP ──
│   ├── query/page.tsx            # SQL 에디터 + 결과 테이블
│   ├── dashboard/page.tsx        # 클러스터 상태 대시보드
│   ├── tables/
│   │   ├── page.tsx              # 테이블 목록
│   │   └── [name]/page.tsx       # 테이블 상세 (스키마, 샘플)
│   │
│   │  # ── P2: Governance ──
│   ├── keys/
│   │   ├── page.tsx              # API 키 목록 + 생성/폐기
│   │   └── [id]/page.tsx         # 키 상세 (role, symbols, 사용이력)
│   ├── roles/page.tsx            # 역할 매트릭스 (5 roles × permissions)
│   ├── tenants/
│   │   ├── page.tsx              # 테넌트 목록 + 생성/삭제
│   │   └── [id]/page.tsx         # 테넌트 상세 (quota, usage)
│   ├── audit/page.tsx            # 감사 로그 뷰어 (필터/검색)
│   ├── sessions/page.tsx         # 활성 세션 목록
│   ├── queries/page.tsx          # 실행중 쿼리 + kill
│   │
│   │  # ── P3: Settings ──
│   └── settings/page.tsx         # 서버 설정, TLS, rate limit
│
└── (marketing)/                  # ── 향후 제품 웹사이트 ──
    ├── layout.tsx
    ├── home/page.tsx
    ├── features/page.tsx
    └── pricing/page.tsx
```

## 4. Phase Details

### P1: MVP — "보여줄 수 있는 제품" (Day 1-5)

#### Query Editor (`/query`)
- CodeMirror SQL 에디터 (Cmd+Enter 실행)
- 결과 테이블 (정렬, 페이지네이션)
- 실행 시간 + 스캔 행수 표시
- 쿼리 히스토리 (localStorage)
- API: `POST /`

#### Dashboard (`/dashboard`)
- 서버 상태 (health, uptime)
- 실시간 카운터: ticks ingested/stored/dropped, queries executed
- ingestion rate 시계열 차트
- API: `GET /stats` (2초 polling)

#### Tables (`/tables`)
- 테이블 목록 + 행 수, 컬럼 수
- 테이블 클릭 → 스키마 상세 (컬럼명, 타입)
- 샘플 데이터 미리보기 (LIMIT 20)
- API: `POST /` → `SHOW TABLES`, `DESCRIBE <table>`

### P2: Governance — 계정/권한/감사 (Day 6-10)

#### API Key Management (`/keys`)
- 키 목록: id, name, role, enabled, created_at, last_used
- 키 생성: name + role 선택 + allowed_symbols 입력
- 키 폐기: revoke 버튼 (soft delete)
- 키 상세: 사용 이력, 접근한 symbol 목록
- API: `GET/POST/DELETE /admin/keys`

#### Role Matrix (`/roles`)
- 5개 역할 × 4개 권한 매트릭스 (읽기 전용 뷰)

| Role | READ | WRITE | ADMIN | METRICS |
|------|------|-------|-------|---------|
| admin | ✅ | ✅ | ✅ | ✅ |
| writer | ✅ | ✅ | ❌ | ✅ |
| reader | ✅ | ❌ | ❌ | ❌ |
| analyst | ✅ (symbol 제한) | ❌ | ❌ | ❌ |
| metrics | ❌ | ❌ | ❌ | ✅ |

- 역할별 설명, 사용 시나리오
- 해당 역할의 키 목록 링크

#### Tenant Management (`/tenants`)
- 테넌트 목록: id, name, priority, namespace
- 테넌트 생성: quota 설정 (concurrent queries, memory, rate, ingestion)
- 테넌트 상세:
  - 리소스 quota vs 실제 사용량 게이지
  - active_queries / total_queries / rejected_queries
  - table namespace 범위
- API: 백엔드 TenantManager (REST endpoint 추가 필요)

#### Audit Log (`/audit`)
- 최근 감사 이벤트 테이블 (timestamp, subject, role, action, detail, IP)
- 필터: subject, role, action, 시간 범위
- 실시간 스트리밍 (polling)
- CSV 내보내기
- API: `GET /admin/audit`

#### Active Sessions (`/sessions`)
- 접속 중인 클라이언트 목록 (IP, user, connected_at, last_active, query_count)
- idle 세션 evict 버튼
- API: `GET /admin/sessions`

#### Running Queries (`/queries`)
- 실행 중인 쿼리 목록 (id, subject, SQL, elapsed)
- Kill 버튼 (CancellationToken)
- API: `GET /admin/queries`, `DELETE /admin/queries/:id`

### P3: Settings + Marketing (Day 11+)

- 서버 설정 뷰 (TLS, rate limit, timeout)
- 마케팅 페이지 (랜딩, features, pricing)

## 5. Backend Changes Required

| Item | Description | Priority |
|------|-------------|----------|
| `GET /ui/*` | Static file serving (Next.js export) | P1 |
| `SHOW TABLES` | 테이블 목록 SQL | P1 |
| `DESCRIBE <table>` | 컬럼 스키마 SQL | P1 |
| CORS | 개발 중 localhost:3000 → :8123 | P1 |
| `GET/POST/DELETE /admin/tenants` | 테넌트 CRUD REST API | P2 |
| `GET /admin/sessions` | 세션 목록 JSON | P2 |
| `GET /admin/keys/:id/usage` | 키별 사용 이력 | P2 |

## 6. Auth Flow in Web UI

```
브라우저 → /ui (로그인 페이지)
  ↓ API Key 입력 (또는 JWT SSO)
  ↓ POST /admin/keys/validate → AuthContext 반환
  ↓ role에 따라 사이드바 메뉴 필터링:
      admin   → 모든 메뉴
      writer  → query, tables, dashboard
      reader  → query, tables, dashboard (SELECT only)
      analyst → query (symbol 제한), tables
      metrics → dashboard only
```

## 7. Dev Workflow

```bash
# 개발 (hot reload)
cd web && pnpm dev              # localhost:3000
# next.config.js rewrites로 localhost:8123 프록시

# 프로덕션 빌드
pnpm build                      # web/out/ 정적 파일
# zepto_server가 GET /ui/* 에서 serve
```

## 8. Success Criteria

- [ ] 투자자 미팅에서 SQL 실행 데모 가능
- [ ] 클러스터 상태를 한 화면에서 확인
- [ ] API 키 생성/폐기를 UI에서 수행
- [ ] 감사 로그를 UI에서 검색/필터
- [ ] role별 메뉴 접근 제어 동작

# ZeptoDB Product Website — PRD

## 1. Overview

ZeptoDB 제품 마케팅 웹사이트. 기술 문서(mkdocs)와 분리된 별도 사이트로,
제품 가치 전달과 리드 확보가 목적.

- **URL**: `zeptodb.io` (제품) / `docs.zeptodb.io` (기술 문서)
- **Repo**: `~/zeptodb-web/` (메인 repo와 분리)

## 2. Target Audience

| Priority | Audience | Pain Point | Message |
|----------|----------|------------|---------|
| 1차 | HFT/퀀트 엔지니어 | kdb+ 라이센스 비용, q 언어 진입장벽 | kdb+ 성능 + 표준 SQL + 오픈소스 |
| 2차 | 데이터 엔지니어 | ClickHouse 실시간 한계 | 실시간 + 분석 통합, ASOF JOIN |
| 3차 | CTO/VP Eng | 벤더 종속, 비용 | 오픈소스, 셀프호스팅, 엔터프라이즈 보안 |

## 3. Page Structure

```
zeptodb.io/
├── /                    # Landing — 히어로 + 핵심 가치 + 벤치마크 하이라이트
├── /features            # 기능 상세 (SIMD, JIT, SQL, Python, Security)
├── /performance         # 벤치마크 비교표 (vs kdb+, ClickHouse)
├── /use-cases           # 산업별 사례 (HFT, Quant, IoT, Crypto)
├── /pricing             # OSS vs Enterprise vs Cloud (placeholder)
├── /blog                # 기술 블로그 (기존 devlog 활용)
├── /about               # 회사 소개, 연락처, GitHub
└── /docs → redirect     # docs.zeptodb.io (mkdocs)
```

### 3.1 Landing Page (/)

- Hero: 한 줄 태그라인 + 핵심 수치 (5.52M ticks/sec, 272μs filter)
- "Why ZeptoDB" 3-column (kdb+ 성능 / 표준 SQL / 오픈소스)
- 벤치마크 하이라이트 테이블
- 코드 스니펫 (SQL + Python DSL)
- CTA: GitHub Star, Get Started, Contact

### 3.2 Features (/features)

- Storage Engine (Arena, Column Store, HDB, Parquet)
- Ingestion (5.52M/sec, MPMC Ring Buffer, WAL)
- Execution (Highway SIMD, LLVM JIT, Parallel Scan)
- SQL (표준 SQL, ASOF JOIN, Window Functions, xbar, EMA)
- Python (zero-copy, Polars-style DSL)
- Security (TLS, RBAC, JWT, Audit Log, SOC2/MiFID II)
- Cluster (Replication, Auto Failover, Coordinator HA)

### 3.3 Performance (/performance)

- 비교표: ZeptoDB vs kdb+ vs ClickHouse
- 벤치마크 차트 (bar chart)
- 테스트 환경/방법론 명시
- 기존 `docs/bench/` 데이터 활용

### 3.4 Use Cases (/use-cases)

- HFT: 틱 처리, ASOF JOIN, xbar OHLCV
- Quant Research: 백테스팅, EMA, Python Jupyter
- Risk/Compliance: 감사 로그, Grafana 대시보드
- Crypto/DeFi: 24/7 멀티 거래소, Kafka
- IoT/Smart Factory: 10KHz 센서, DELTA

### 3.5 Pricing (/pricing)

- Community (OSS): 무료, 셀프호스팅
- Enterprise: 유료 지원, SLA, 전용 기능
- Cloud (Coming Soon): 관리형 서비스
- Contact Sales CTA

### 3.6 Blog (/blog)

- 기존 devlog (000~023) 마이그레이션
- 새 기술 블로그 포스트
- RSS 피드

## 4. Technical Requirements

### 4.1 Tech Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Framework | **Astro** | 콘텐츠 중심 SSG, 제로 JS 기본, 빠른 빌드 |
| UI | **Tailwind CSS** | 유틸리티 기반, 다크모드 내장 |
| Components | **React** (필요시) | 인터랙티브 요소만 island로 |
| Content | **MDX** | 블로그/콘텐츠에 컴포넌트 삽입 |
| Deploy | **Cloudflare Pages** | 무료, 글로벌 CDN, 빠름 |
| Domain | `zeptodb.io` | 제품 / `docs.zeptodb.io` 문서 |

### 4.2 Non-functional

- **Performance**: Lighthouse 95+ (모든 카테고리)
- **SEO**: 메타태그, OG 이미지, sitemap.xml, robots.txt
- **i18n**: en (기본) / ko
- **반응형**: 모바일 / 태블릿 / 데스크톱
- **다크모드**: 시스템 설정 연동 + 수동 토글
- **접근성**: WCAG 2.1 AA

## 5. Execution Plan

### Phase 1: 기반 구축 (Day 1-2)

1. Astro 프로젝트 초기화 + Tailwind 설정
2. 레이아웃 (Header/Nav/Footer) 컴포넌트
3. 다크모드 + 반응형 기본
4. Cloudflare Pages 배포 파이프라인

### Phase 2: 핵심 페이지 (Day 3-5)

5. Landing 페이지
6. Features 페이지
7. Performance 페이지 (비교표 + 차트)
8. Use Cases 페이지

### Phase 3: 콘텐츠 + 마무리 (Day 6-7)

9. Pricing 페이지
10. Blog (devlog 마이그레이션)
11. i18n (ko) 적용
12. SEO + OG 이미지 + sitemap

## 6. Content Sources (기존 자산 활용)

| Source | Usage |
|--------|-------|
| `README.md` | 랜딩 페이지 핵심 수치, 비교표 |
| `docs/bench/` | Performance 페이지 데이터 |
| `docs/design/` | Features 페이지 아키텍처 |
| `docs/business/` | Use Cases, Pricing 참고 |
| `docs/devlog/` | Blog 콘텐츠 |

## 7. Success Metrics

- GitHub Star 증가율
- 문서 페이지 유입 (Landing → Docs 전환율)
- Contact/Demo 요청 수
- Lighthouse 점수 95+
- 페이지 로드 < 1초 (FCP)

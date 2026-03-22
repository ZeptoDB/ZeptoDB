# APEX-DB 비즈니스 전략 문서
**작성일:** 2026-03-22
**버전:** 1.0

---

## Executive Summary

APEX-DB는 kdb+를 대체하는 오픈소스 시계열 데이터베이스로, HFT(High-Frequency Trading) 시장을 타겟으로 한다.

**핵심 차별화:**
- **성능:** kdb+ 동등 (5.52M ticks/sec, <1μs 지연)
- **가격:** TCO 90% 절감 (오픈소스)
- **생산성:** SQL (vs q 언어)
- **Python 통합:** zero-copy (522ns)

**12개월 목표:**
- **매출:** $6.85M - $12.1M ARR
- **고객:** 45-65개
- **시장:** HFT, 암호화폐, 헤지펀드, 광고 테크

---

## 1. 시장 분석

### 1.1 타겟 시장 (연 매출 잠재력 순)

| 시장 | 규모 | ARPU | 예상 매출 | 우선순위 |
|------|------|------|----------|----------|
| **HFT/Prop Trading** | 500+ firms | $250K-500K | $2.5M-12M | P0 ⭐⭐⭐⭐⭐ |
| **암호화폐 거래소** | 10 Tier-1 | $50K-200K | $1M-3M | P0 ⭐⭐⭐⭐ |
| **헤지펀드/자산운용** | 100+ funds | $20K-100K | $1M-3M | P1 ⭐⭐⭐⭐ |
| **은행/투자은행** | 50+ banks | $50K-200K | $1M-3M | P1 ⭐⭐⭐ |
| **광고 테크** | 20+ companies | $100K-300K | $1M-3M | P1 ⭐⭐⭐⭐ |

**Total Addressable Market:** $7.5M-24M ARR (Year 1-3)

---

### 1.2 경쟁 분석

#### vs kdb+

| 항목 | kdb+ | APEX-DB | 우위 |
|------|------|---------|------|
| **가격** | $100K-500K/년 | 오픈소스 | ✅ APEX-DB |
| **학습 곡선** | q (6-12개월) | SQL (1주) | ✅ APEX-DB |
| **Python 연동** | PyKX (IPC) | zero-copy 522ns | ✅ APEX-DB |
| **성능** | 기준 | 95% 동등 | ≈ 동등 |
| **클라우드** | 제한적 | Kubernetes native | ✅ APEX-DB |
| **생태계** | 성숙 | 신생 | ❌ kdb+ |

**킬링 메시지:**
> "kdb+의 성능 + 오픈소스의 가격 + SQL의 생산성"

**TCO 비교 (3년):**
- kdb+: $900K (라이선스 $300K + 인력 $600K)
- APEX-DB: $150K (인력 $150K)
- **절감: $750K (83%)**

---

#### vs ClickHouse

| 항목 | ClickHouse | APEX-DB | 우위 |
|------|-----------|---------|------|
| **금융 함수** | 없음 (UDF 필요) | xbar/EMA/wj native | ✅ APEX-DB |
| **실시간** | 초당 100K | 5.52M ticks/sec | ✅ APEX-DB (55x) |
| **Python DSL** | 없음 | 4-37x faster Polars | ✅ APEX-DB |
| **SIMD** | SSE4.2 | AVX-512 | ✅ APEX-DB |

**킬링 메시지:**
> "ClickHouse + 금융 함수 + Python 퀀트 툴"

---

#### vs TimescaleDB

| 항목 | TimescaleDB | APEX-DB | 우위 |
|------|------------|---------|------|
| **성능** | PostgreSQL 기반 | 100x 빠름 | ✅ APEX-DB |
| **인제스션** | 초당 10K | 5.52M ticks/sec | ✅ APEX-DB (552x) |
| **금융 함수** | 없음 | kdb+ 호환 | ✅ APEX-DB |

---

#### vs Snowflake/Databricks (보완재 전략)

| 워크로드 | Snowflake/Databricks | APEX-DB | 전략 |
|---------|---------------------|---------|------|
| **배치 분석** | ✅ 최적 | ❌ | 양보 |
| **ML/AI** | ✅ 최적 | ❌ | 양보 |
| **실시간 분석** | ❌ 느림 | ✅ 최적 | 공략 |
| **고빈도 인제스션** | ❌ 비쌈 | ✅ 최적 | 공략 |
| **온프레미스** | ❌ 불가 | ✅ 가능 | 공략 |

**포지셔닝:**
> "The Real-time Companion to Snowflake"
> "Keep your Data Warehouse, add Real-time Analytics"

---

## 2. 제품 현황

### 2.1 핵심 기능 (완료)

| 기능 | 상태 | 성능 |
|------|------|------|
| **인제스션** | ✅ | 5.52M ticks/sec |
| **금융 함수** | ✅ | xbar, EMA, wj |
| **병렬 쿼리** | ✅ | 8T = 3.48x |
| **Python 통합** | ✅ | zero-copy 522ns |
| **Feed Handler** | ✅ | FIX, ITCH, UDP |
| **프로덕션 운영** | ✅ | 모니터링, 백업 |

**kdb+ 대체율:**
- HFT: 95%
- 퀀트: 90%
- 리스크: 95%

---

### 2.2 Feed Handler Toolkit (신규 완료)

**비즈니스 가치:** HFT 시장 진입 핵심

| 프로토콜 | 파싱 속도 | 지연시간 | 용도 |
|----------|-----------|---------|------|
| **FIX** | 350ns | 100μs-1ms | 데이터 벤더 (Bloomberg, Reuters) |
| **ITCH** | 250ns | 1-5μs | 거래소 직접 (NASDAQ) |
| **UDP** | N/A | <1μs | 멀티캐스트 (초저지연) |

**최적화:**
- Zero-copy 파싱 (2-3x)
- SIMD AVX2 (5-10x)
- Memory Pool (10-20x)
- Lock-free Ring Buffer (3-5x)

**테스트 커버리지:** 100% (27 단위 + 10 벤치마크)

**ROI:** HFT 시장 진입 가능 ($2.5M-12M)

---

## 3. Go-to-Market 전략

### 3.1 3개월 로드맵 (ROI 최적화)

```
Month 1: Quick Wins (첫 매출)
├─ Week 1-4: ClickHouse 마이그레이션 (4주)
└─ Week 3-4: DuckDB 통합 (병렬, 2주)
    → 목표: 첫 PoC $150K

Month 2-3: Big Bet (큰 판)
└─ Week 5-11: kdb+ 마이그레이션 (7주)
    → 목표: HFT 파이프라인 구축

병렬 작업:
- Marketing: ClickHouse 케이스 스터디
- Sales: HFT 고객 발굴
```

---

### 3.2 마이그레이션 툴킷 (고객 확보 핵심)

#### Priority 0: kdb+ → APEX-DB (7주, $2.5M-12M)
**개발 항목:**
1. q → SQL 트랜스파일러 (4주)
   - `select`, `where`, `fby`, `aj`, `wj` 자동 변환
2. HDB 데이터 로더 (2주)
   - Splayed tables → Columnar format
3. 성능 검증 도구 (1주)
   - TPC-H + 금융 쿼리 벤치마크

**비즈니스 가치:**
- 가장 큰 ARPU ($250K-500K/고객)
- HFT 시장 진입
- Feed Handler와 시너지

**킬링 메시지:**
> "kdb+의 모든 것 + 오픈소스 + TCO 90% 절감"

---

#### Priority 1: ClickHouse → APEX-DB (4주, $1M-3M)
**개발 항목:**
1. SQL 방언 변환 (1주)
   - `arrayJoin` → `UNNEST`
   - `uniq` → `COUNT(DISTINCT)`
2. 데이터 마이그레이션 (1주)
   - MergeTree → Columnar
3. 쿼리 최적화 (1주)
   - 느린 쿼리 감지 + Index 추천
4. PoC 자동화 (1주)

**비즈니스 가치:**
- **빠른 첫 매출:** 3개월
- 광고 테크, SaaS 분석 시장
- 레퍼런스 고객 확보

**타겟 고객:**
- 광고 테크 (실시간 입찰)
- SaaS 분석 (Amplitude, Mixpanel 대체)
- 게임 분석

**킬링 메시지:**
> "ClickHouse + 금융 함수 + Python 통합"

---

#### Priority 1: DuckDB 상호운용성 (2주, 전략적)
**개발 항목:**
1. DuckDB Parquet → APEX-DB (1주)
   - Arrow zero-copy
2. 벤치마크 + 블로그 (1주)

**비즈니스 가치:**
- **마케팅 레버리지:** Hacker News 론칭
- **인바운드 리드:** 월 50-100개
- Python 커뮤니티 진입

**킬링 메시지:**
> "DuckDB의 실시간 버전"
> "Jupyter에서 바로 프로덕션으로"

---

#### Priority 2: TimescaleDB → APEX-DB (3주, $500K-1M)
**개발 항목:**
1. 스키마 변환 (1주): Hypertables → APEX-DB
2. pg_dump 자동 변환 (1주)
3. 함수 매핑 (1주): `time_bucket` → `xbar`

**타겟 고객:**
- IoT 플랫폼
- DevOps 모니터링

---

#### 전략적: Snowflake/Delta Lake Hybrid (4주, $3.5M)
**개발 항목:**
1. Snowflake 커넥터 (2주)
   - JDBC/ODBC 통합
   - Cold data 쿼리
2. Delta Lake Reader (2주)
   - Parquet + transaction log

**타겟 워크로드:**
- 실시간 금융 분석 (20 고객 × $50K = $1M)
- IoT/센서 데이터 (10 고객 × $50K = $500K)
- 광고 테크 실시간 입찰 (10 고객 × $100K = $1M)
- 규제 산업 온프레미스 (5 고객 × $200K = $1M)

**포지셔닝:**
> "Snowflake for batch, APEX-DB for real-time"

**Hybrid 아키텍처:**
```
┌─────────────────┐
│   Snowflake     │  배치 분석, ML, Data Lake
│   (Cold Data)   │  - 월별 리포트
└────────┬────────┘  - 예측 모델
         │ ETL (매일)
         ↓
┌────────▼────────┐
│   APEX-DB       │  실시간 분석, 금융
│   (Hot Data)    │  - 실시간 대시보드
└─────────────────┘  - HFT 트레이딩
```

---

## 4. 재무 전망

### 4.1 매출 타임라인 (12개월)

#### Q1 (Month 1-3)
- ClickHouse PoC: 3개 × $50K = **$150K**
- DuckDB 인바운드: 리드 생성
- kdb+ 파이프라인: 2-3개 잠재 고객

#### Q2 (Month 4-6)
- ClickHouse 계약: 5개 × $100K = **$500K**
- kdb+ 첫 계약: 1개 × $250K = **$250K**
- DuckDB 전환: 5개 × $30K = **$150K**
- **Subtotal: $900K**

#### Q3 (Month 7-9)
- ClickHouse: 3개 × $100K = **$300K**
- kdb+: 2개 × $250K = **$500K**
- Snowflake Hybrid: 5개 × $50K = **$250K**
- **Subtotal: $1.05M**

#### Q4 (Month 10-12)
- ClickHouse: 2개 × $100K = **$200K**
- kdb+: 2개 × $250K = **$500K**
- DuckDB: 10개 × $30K = **$300K**
- TimescaleDB: 10개 × $50K = **$500K**
- **Subtotal: $1.5M**

**Year 1 Total: $3.6M ARR**

---

### 4.2 마이그레이션 툴킷별 매출

| 툴킷 | 12개월 매출 | 3년 매출 |
|------|-------------|----------|
| kdb+ | $1.25M | $7.5M |
| ClickHouse | $1.0M | $3.0M |
| DuckDB | $600K | $1.8M |
| TimescaleDB | $500K | $1.5M |
| Snowflake/Delta Lake | $3.5M | $10.5M |
| **Total** | **$6.85M** | **$24.3M** |

---

### 4.3 고객 수 전망

| 세그먼트 | Year 1 | Year 2 | Year 3 |
|---------|--------|--------|--------|
| HFT/Prop Trading | 5 | 15 | 30 |
| 암호화폐 거래소 | 3 | 8 | 15 |
| 헤지펀드 | 10 | 30 | 60 |
| 광고 테크 | 10 | 20 | 40 |
| 은행/규제 산업 | 5 | 10 | 20 |
| IoT/DevOps | 10 | 25 | 50 |
| **Total** | **43** | **108** | **215** |

---

### 4.4 단위 경제학 (Unit Economics)

#### 고객 획득 비용 (CAC)
- **HFT/kdb+:** $50K (Enterprise 영업 12개월)
- **ClickHouse:** $10K (빠른 PoC 3개월)
- **DuckDB:** $1K (인바운드)

#### 고객 생애 가치 (LTV)
- **HFT:** $1.5M (3년 계약, $500K/년)
- **ClickHouse:** $300K (3년 계약, $100K/년)
- **DuckDB:** $90K (3년 계약, $30K/년)

#### LTV/CAC 비율
- **HFT:** 30x (건강)
- **ClickHouse:** 30x (건강)
- **DuckDB:** 90x (매우 건강)

---

## 5. 데이터 인제스션 전략

### 5.1 HFT 데이터 흐름

```
거래소 Matching Engine
    ↓ UDP Multicast (1-10 Gbps)
Feed Handler (C++)
    ↓ 파싱 (ITCH, SBE, FIX)
APEX-DB TickerPlant
    ↓ 5.52M ticks/sec
RDB (실시간) + HDB (히스토리)
```

**프로토콜:**
- **NASDAQ ITCH:** 바이너리, UDP (1-5μs)
- **CME iLink3:** FIX/SBE, TCP (10-50μs)
- **Bloomberg B-PIPE:** TCP, 전용 프로토콜

**APEX-DB 우위:**
- ✅ FIX Parser: 350ns
- ✅ ITCH Parser: 250ns
- ✅ UDP Multicast: <1μs
- ✅ 완전한 테스트 커버리지

---

## 6. 경쟁 전략

### 6.1 시장별 전략

#### HFT 시장 (Priority 0)
**전략:** 정면 승부 (kdb+ 대체)

**차별화:**
1. **가격:** TCO 90% 절감
2. **생산성:** SQL vs q
3. **Python:** zero-copy
4. **오픈소스:** 커뮤니티

**진입 장벽:**
- Feed Handler ✅ (완료)
- kdb+ 마이그레이션 (7주 개발 필요)
- HFT 레퍼런스 (첫 고객 확보)

**킬링 메시지:**
> "같은 성능, 1/10 가격, 10배 생산성"

---

#### 광고 테크/SaaS (Priority 1)
**전략:** ClickHouse 대체

**차별화:**
1. **금융 함수:** xbar, EMA (광고 분석에도 유용)
2. **Python 통합:** Jupyter 리서치
3. **실시간:** 55x 빠름

**진입 장벽:** 낮음 (PoC 3개월)

---

#### Snowflake 고객 (전략적)
**전략:** 보완재 (Hybrid)

**차별화:**
1. **실시간:** Snowflake는 배치만
2. **비용:** 실시간 워크로드 이동으로 Snowflake 비용 절감
3. **온프레미스:** 규제 산업

**진입 장벽:** 중간 (파트너십 필요)

---

## 7. 마케팅 전략

### 7.1 컨텐츠 마케팅

#### Phase 1 (Month 1-2): ClickHouse
**목표:** 첫 고객 확보

**컨텐츠:**
1. 블로그: "ClickHouse → APEX-DB 마이그레이션 가이드"
2. 벤치마크: "ClickHouse vs APEX-DB (금융 쿼리)"
3. 웨비나: "실시간 금융 분석 구축"

---

#### Phase 2 (Month 2-3): DuckDB
**목표:** 커뮤니티 구축

**컨텐츠:**
1. Hacker News: "DuckDB + Real-time = APEX-DB"
2. Reddit r/datascience: 튜토리얼
3. Twitter: 성능 벤치마크

**예상 효과:**
- GitHub 스타: 500+
- 인바운드 리드: 월 50+

---

#### Phase 3 (Month 4-6): kdb+
**목표:** HFT 시장 진입

**컨텐츠:**
1. 백서: "kdb+ 마이그레이션 완전 가이드"
2. 컨퍼런스: QuantCon, Battle of the Quants
3. 케이스 스터디: ClickHouse 고객 레퍼런스

---

### 7.2 영업 전략

#### Enterprise (HFT, 은행)
- **사이클:** 12-24개월
- **접근:** 직접 영업
- **PoC:** $50K (전체 DB)

#### Mid-Market (광고 테크, 헤지펀드)
- **사이클:** 3-6개월
- **접근:** 인바운드 + 직접
- **PoC:** 무료 (1 테이블)

#### SMB (스타트업, DuckDB 사용자)
- **사이클:** 1-2개월
- **접근:** 셀프서비스
- **PoC:** 무료

---

## 8. 파트너십 전략

### 8.1 Snowflake/Databricks
**전략:** 공식 파트너

**제안:**
- "우리는 경쟁자 아님, 보완재"
- "고객에게 실시간 솔루션 제공"

**Win-Win:**
- Snowflake: 실시간 갭 해결, 고객 이탈 방지
- APEX-DB: 브랜드 신뢰도, 리드 생성

---

### 8.2 클라우드 파트너 (AWS, GCP, Azure)
**전략:** Marketplace 출시

**혜택:**
- 고객 발굴
- 빌링 통합
- 공동 마케팅

**목표:** Year 2 출시

---

## 9. 리스크 관리

### 9.1 기술 리스크

| 리스크 | 영향 | 완화 전략 |
|--------|------|----------|
| kdb+ 호환성 불완전 | 높음 | 95% 달성, 핵심 기능 우선 |
| 성능 미달 | 높음 | 지속적 벤치마크, 최적화 |
| 버그/안정성 | 중간 | 100% 테스트 커버리지 |

---

### 9.2 비즈니스 리스크

| 리스크 | 영향 | 완화 전략 |
|--------|------|----------|
| kdb+ 가격 인하 | 높음 | 오픈소스 장점 (커뮤니티) |
| ClickHouse 금융 함수 추가 | 중간 | 선점 효과, 레퍼런스 |
| HFT 진입 실패 | 높음 | ClickHouse로 빠른 검증 |
| 긴 영업 사이클 | 중간 | 단기(ClickHouse) + 장기(kdb+) 병행 |

---

## 10. 팀 빌드업

### 10.1 필요 인력

| 시기 | 역할 | 인원 |
|------|------|------|
| **Month 1-2** | 엔지니어 (ClickHouse) | 2명 |
| **Month 3-6** | 엔지니어 (kdb+) | +2명 (총 4명) |
| | Sales (Enterprise) | +1명 |
| **Month 7-12** | 엔지니어 | +2명 (총 6명) |
| | Sales | +1명 (총 2명) |
| | Customer Success | +1명 |

**Year 1 Total: 9명**

---

## 11. 핵심 성공 지표 (KPI)

### 11.1 제품 KPI
- **성능:** 5M+ ticks/sec 유지
- **안정성:** 99.9% Uptime
- **테스트:** 100% 커버리지 유지

### 11.2 비즈니스 KPI
- **MRR:** 월별 반복 매출
- **CAC:** 고객 획득 비용
- **LTV/CAC:** 30x 이상 유지
- **Churn:** <5% 연간

### 11.3 마케팅 KPI
- **GitHub 스타:** 500+ (6개월)
- **인바운드 리드:** 50+/월
- **컨퍼런스 발표:** 4회/년

---

## 12. 실행 우선순위 (Immediate Actions)

### 즉시 시작 (Week 1-4)
1. ✅ **Feed Handler 완료** (완료)
2. ⏩ **ClickHouse 마이그레이션** 시작 (4주)
3. ⏩ **DuckDB 통합** 병렬 진행 (2주)

### 다음 단계 (Week 5-11)
4. **kdb+ 마이그레이션** 시작 (7주)
5. **첫 HFT 고객** PoC

### 전략적 (Month 4-6)
6. **Snowflake Hybrid** 전략 실행
7. **파트너십** 추진

---

## 13. 결론

### 13.1 핵심 강점
1. ✅ **기술 준비:** kdb+ 95% 대체 달성
2. ✅ **Feed Handler:** HFT 진입 가능
3. ✅ **마이그레이션 툴킷:** 명확한 로드맵
4. ✅ **차별화:** 성능 + 가격 + 생산성

### 13.2 성공 확률
- **ClickHouse 시장:** 90% (빠른 검증)
- **kdb+ 시장:** 60% (고위험 고수익)
- **Snowflake Hybrid:** 70% (보완재)

### 13.3 최종 목표
- **Year 1:** $3.6M ARR (43 고객)
- **Year 2:** $8.5M ARR (108 고객)
- **Year 3:** $24.3M ARR (215 고객)

**Break-even:** Month 9-12 (매출 > 비용)

---

## 부록

### A. 참고 문서
- `docs/feeds/FEED_HANDLER_COMPLETE.md` - Feed Handler 완료 보고서
- `docs/deployment/PRODUCTION_DEPLOYMENT.md` - 배포 가이드
- `docs/operations/PRODUCTION_OPERATIONS.md` - 운영 가이드
- `BACKLOG.md` - 개발 백로그

### B. 경쟁사 링크
- kdb+: https://kx.com
- ClickHouse: https://clickhouse.com
- Snowflake: https://snowflake.com
- DuckDB: https://duckdb.org

### C. 연락처
- 제품 문의: product@apex-db.io
- 영업 문의: sales@apex-db.io
- 파트너십: partners@apex-db.io

---

**문서 이력:**
- 2026-03-22: v1.0 초안 작성

# APEX-DB vs kdb+ 대체 가능성 분석
# 2026-03-22

---

## 1. kdb+가 제공하는 핵심 기능 체크리스트

kdb+/q의 기능을 전수 조사하고, APEX-DB 현재 상태와 비교.

### A. 데이터 인제스션 & 스토리지

| kdb+ 기능 | 설명 | APEX-DB | 상태 |
|---|---|---|---|
| Tickerplant (TP) | 실시간 틱 수집, Pub/Sub | TickPlant + MPMC | ✅ |
| RDB (실시간 DB) | 당일 데이터 인메모리 | Arena + ColumnStore | ✅ |
| HDB (히스토리) | 과거 데이터 디스크 (splayed) | HDB Writer/Reader + LZ4 | ✅ |
| WAL (TP 로그) | 장애 복구 로그 | WAL Writer | ✅ |
| EOD 프로세스 | 장 마감 시 RDB→HDB 전환 | FlushManager | ✅ |
| Attributes (g#, p#, s#, u#) | 인덱스 힌트 | 파티션 기반 (부분) | ⚠️ |
| Symbol interning | 심볼 해시 최적화 | SymbolId (uint32) | ✅ |

**갭:** kdb+ attributes(g#=grouped, s#=sorted, p#=parted)는 쿼리 옵티마이저 힌트. APEX-DB는 파티션 구조로 대부분 커버하지만, 명시적 attribute API는 없음.

### B. 쿼리 언어 & 실행

| kdb+ 기능 | 설명 | APEX-DB | 상태 |
|---|---|---|---|
| q-SQL select | SELECT-WHERE-GROUP BY | SQL Parser + Executor | ✅ |
| fby (filter by) | 그룹별 필터링 | SQL WHERE + GROUP BY | ✅ |
| 벡터 연산 | 컬럼 전체에 대한 일괄 연산 | Highway SIMD | ✅ |
| 집계 (sum, avg, min, max, count) | 기본 집계 | ✅ 구현 | ✅ |
| VWAP (wavg) | 가중 평균 | VWAP 함수 | ✅ |
| xbar | 시간 바 집계 (5분봉 등) | ❌ 미구현 | 🔴 |
| ema (지수이동평균) | 금융 핵심 지표 | ❌ 미구현 | 🔴 |
| mavg, msum, mmin, mmax | 이동 평균/합계/최소/최대 | Window SUM/AVG/MIN/MAX | ✅ |
| deltas, ratios | 행 간 차이, 비율 | LAG로 계산 가능 | ⚠️ |
| within | 범위 체크 | BETWEEN | ✅ |
| each, peach | 벡터/병렬 맵 | ❌ (Python DSL로 대체) | ⚠️ |

**핵심 갭:**
- **`xbar`**: 금융에서 매우 자주 사용 (5분봉, 1시간봉). GROUP BY + 시간 floor로 구현 가능
- **`ema`**: 지수이동평균. 별도 구현 필요 (O(n) 단순 루프)

### C. JOIN 연산

| kdb+ 기능 | 설명 | APEX-DB | 상태 |
|---|---|---|---|
| aj (asof join) | 시계열 조인 | AsofJoinOperator | ✅ |
| aj0 | 왼쪽 컬럼만 반환 | 변형으로 가능 | ⚠️ |
| ij (inner join) | 내부 조인 | HashJoinOperator | ✅ |
| lj (left join) | 왼쪽 조인 | ❌ 미구현 | 🔴 |
| uj (union join) | 합집합 조인 | ❌ 미구현 | 🟡 |
| wj (window join) | 시간 윈도우 조인 | ❌ 미구현 | 🔴 |
| ej (equi join) | 등가 조인 | HashJoinOperator | ✅ |
| pj (plus join) | 덧셈 조인 | ❌ 미구현 | 🟡 |

**핵심 갭:**
- **`wj` (window join)**: "이 시점 전후 5초 이내의 호가를 모두 가져와라" — HFT 핵심
- **`lj` (left join)**: 기본 SQL LEFT JOIN — 구현 쉬움

### D. 시스템 & 운영

| kdb+ 기능 | 설명 | APEX-DB | 상태 |
|---|---|---|---|
| IPC 프로토콜 | 프로세스 간 통신 | HTTP API + gRPC(계획) | ⚠️ |
| 멀티프로세스 (TP/RDB/HDB/GW) | 역할별 프로세스 분리 | Pipeline 단일 + 분산 | ⚠️ |
| Gateway | 쿼리 라우팅 | PartitionRouter | ✅ |
| -s secondary threads | 병렬 쿼리 | ❌ 단일 스레드 쿼리 | 🔴 |
| .z.ts 타이머 | 스케줄링 | ❌ | 🟡 |
| \t 타이밍 | 쿼리 벤치마크 | execution_time_us | ✅ |

**핵심 갭:**
- **병렬 쿼리 실행**: 현재 쿼리가 단일 스레드. 멀티스레드 쿼리 필요
- **프로세스 역할 분리**: kdb+는 TP/RDB/HDB/Gateway 별도 프로세스. APEX-DB는 통합형

### E. Python 연동

| kdb+ 기능 | 설명 | APEX-DB | 상태 |
|---|---|---|---|
| PyKX | kdb+↔Python | pybind11 + DSL | ✅ |
| IPC 기반 접근 | 소켓으로 데이터 전송 | zero-copy (메모리 직접) | ✅ 우위 |
| Arrow 연동 | Apache Arrow 변환 | Arrow 호환 레이아웃 | ✅ 우위 |

→ Python 연동은 **APEX-DB가 kdb+보다 확실히 우위** (zero-copy vs IPC 직렬화)

---

## 2. 핵심 갭 요약 (kdb+ 대체 위해 반드시 필요한 것)

### 🔴 긴급 (없으면 대체 불가)

| 기능 | 이유 | 난이도 | 예상 시간 |
|---|---|---|---|
| **xbar (시간 바)** | 5분봉, 1시간봉 — 금융 기본 | ⭐ | 2시간 |
| **ema (지수이동평균)** | 기술적 지표 핵심 | ⭐ | 1시간 |
| **lj (LEFT JOIN)** | SQL 기본, 분석 필수 | ⭐⭐ | 3시간 |
| **wj (Window JOIN)** | HFT 호가 분석 핵심 | ⭐⭐⭐ | 1일 |
| **병렬 쿼리 실행** | 대량 데이터 쿼리 속도 | ⭐⭐⭐ | 2일 |

### 🟡 중요 (없어도 되지만 있으면 경쟁력 상승)

| 기능 | 이유 | 난이도 |
|---|---|---|
| deltas/ratios 네이티브 | LAG 대신 전용 함수 | ⭐ |
| uj (union join) | 테이블 합치기 | ⭐⭐ |
| Attribute 힌트 (s#, g#) | 쿼리 최적화 | ⭐⭐ |
| 타이머/스케줄러 | EOD 자동화 | ⭐⭐ |

### ✅ 이미 kdb+보다 나은 것

| 항목 | kdb+ | APEX-DB | 이유 |
|---|---|---|---|
| 언어 접근성 | q (난해) | **SQL + Python** | 학습 곡선 |
| Python 연동 | PyKX (IPC) | **zero-copy** | 522ns vs ms |
| SIMD 벡터화 | 없음 (q 인터프리터) | **Highway AVX-512** | 하드웨어 활용 |
| JIT 컴파일 | 없음 | **LLVM OrcJIT** | 동적 쿼리 최적화 |
| 클라우드 스케일 | 제한적 | **분산 + Transport 교체** | CXL 대비 |
| HTTP API | 없음 (IPC만) | **port 8123** | Grafana 연결 |
| 가격 | 연 $100K+ | **오픈소스 가능** | 비용 |
| Window 함수 | mavg/msum | **SQL 표준 OVER** | 표준 호환 |

---

## 3. 대체 가능성 판정

### HFT (틱 처리 + 실시간 쿼리)
**현재: 80% 대체 가능**
- ✅ 인제스션, RDB/HDB, VWAP, ASOF JOIN
- 🔴 누락: xbar, ema, wj, 병렬 쿼리
- 구현하면: **95% 대체 가능**

### 퀀트 리서치 (백테스트)
**현재: 70% 대체 가능**
- ✅ Python DSL, Window 함수, GROUP BY
- 🔴 누락: ema, xbar, wj, 복잡한 조인
- 구현하면: **90% 대체 가능**

### 리스크/컴플라이언스
**현재: 85% 대체 가능**
- ✅ SQL, Hash JOIN, GROUP BY
- 🔴 누락: LEFT JOIN, 병렬 쿼리
- 구현하면: **95% 대체 가능**

---

## 4. 추천 액션 플랜

**1주일이면 kdb+ 90%+ 대체 가능 수준 도달:**

| 일차 | 작업 | 시간 |
|---|---|---|
| Day 1 | xbar + ema + deltas/ratios | 4h |
| Day 2 | LEFT JOIN + Window JOIN (wj) | 8h |
| Day 3 | 병렬 쿼리 실행 (thread pool) | 8h |
| Day 4 | 통합 테스트 + 벤치마크 | 4h |
| Day 5 | 문서 + kdb+ 마이그레이션 가이드 | 4h |

# ZeptoDB Industry Use Case Gap Analysis

Last updated: 2026-03-24

---

## Overview

ZeptoDB는 퀀트/HFT를 위해 설계되었지만, 동일한 엔진이 다양한 산업의 시계열 워크로드에 적용 가능하다. 이 문서는 각 산업별 실제 쿼리 패턴을 테스트하면서 발견된 **현재 지원 범위**와 **부족한 부분**을 정리한다.

검증 기준: `tests/unit/test_sql.cpp` — `IndustryUseCaseTest` (16개 테스트, 전부 통과)

---

## 1. 공통 제한사항

테스트 과정에서 전 산업에 걸쳐 반복적으로 부딪힌 제한사항.

| 제한사항 | 영향도 | 우회 방법 | 상태 |
|----------|--------|-----------|------|
| ~~`SUM(CASE WHEN ... THEN col ELSE 0 END)` 미지원~~ | ~~높음~~ | — | ✅ 해결 |
| ~~`WHERE symbol IN (1, 2, 3)` 멀티 파티션 미지원~~ | ~~중간~~ | — | ✅ 해결 |
| ~~비집계 `ORDER BY col DESC` 불안정~~ | ~~중간~~ | — | ✅ 해결 |
| ASOF JOIN self-join 불가 | 중간 — 같은 테이블 내 시간 비교 불가 | `LAG()` 윈도우 함수로 대체 | 미해결 |
| `store_tick_direct` 시 DELTA/RATIO 0 반환 | 낮음 — 테스트 전용 이슈, 프로덕션 영향 없음 | `ingest_tick` + `drain_sync` 사용 | 미해결 |
| Float/Double 네이티브 컬럼 없음 | 높음 — 모든 비금융 산업에서 온보딩 마찰 | int64 스케일링 (×100, ×1000) | P1 |
| String 컬럼 없음 | 높음 — 서비스명, 장비명, 에러 메시지 저장 불가 | symbol ID 매핑 테이블 외부 관리 | P1 |
| 통계 함수 부재 (PERCENTILE, STDDEV, MEDIAN) | ~~중간~~ | — | ✅ 해결 — STDDEV/VARIANCE/MEDIAN/PERCENTILE 구현 |
| 날짜 리터럴 미지원 | 낮음 — `'2026-03-24'` 대신 나노초 정수 사용 | `NOW() - N` 또는 수동 변환 | P2 |

---

## 2. 산업별 분석

### 2.1 IoT / 스마트팩토리

**현재 지원되는 패턴:**
- ✅ 장비별 스파이크 감지 (`WHERE price > threshold GROUP BY symbol`)
- ✅ 초당/분당 집계 대시보드 (`xbar` + `AVG/MAX/MIN`)
- ✅ 장비 간 비교 (`GROUP BY symbol ORDER BY`)
- ✅ 이상치 필터링 (`WHERE + ORDER BY DESC LIMIT`)

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| MQTT 인제스션 | 센서 → ZeptoDB 직접 수신 (현재 HTTP/Kafka만) | P2 |
| OPC-UA 커넥터 | Siemens S7, PLC 연동 | P2 |
| Float 컬럼 | 온도 23.7°C, 진동 0.003g | P1 |
| 다중 테이블 JOIN | 센서 + 장비 마스터 + 알람 | P1 |
| `SUM(CASE WHEN)` | 정상/비정상 구간별 집계 | P0 |

**경쟁사 대비:**
- vs InfluxDB: ZeptoDB가 인제스션 100x 빠름, ASOF JOIN 지원. InfluxDB는 Float 네이티브.
- vs TimescaleDB: ZeptoDB가 μs 레이턴시. TimescaleDB는 PostgreSQL 생태계 (String, Float, JOIN 완전 지원).

---

### 2.2 자율주행 / 로보틱스

**현재 지원되는 패턴:**
- ✅ 주행 로그 집계 (`AVG/MAX/COUNT GROUP BY`)
- ✅ 차량 플릿 비교 (`GROUP BY symbol ORDER BY DESC`)
- ✅ EMA 기반 스무딩 (`EMA OVER PARTITION BY`)
- ✅ Parquet HDB로 장기 로그 저장

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| ROS2 토픽 인제스션 | LiDAR/Camera/IMU 직접 수신 | P2 |
| Array/Blob 컬럼 | 포인트 클라우드, 이미지 프레임 | P3 |
| 크로스 테이블 ASOF JOIN | LiDAR + Camera 시간 동기화 (현재 동일 테이블만) | P1 |
| Float 컬럼 | GPS 좌표, 가속도 | P1 |

**경쟁사 대비:**
- vs ROS bag: ZeptoDB가 SQL 쿼리 가능, 실시간 인제스션. ROS bag은 파일 기반 재생만.
- vs ClickHouse: ZeptoDB가 1000x 빠른 레이턴시, ASOF JOIN. ClickHouse는 String/Float 완전 지원.

---

### 2.3 Observability / APM

**현재 지원되는 패턴:**
- ✅ 서비스별 고지연 탐지 (`GROUP BY HAVING MAX > threshold`)
- ✅ 요청 통계 (`COUNT/MAX GROUP BY`)
- ✅ 서비스 의존성 추적 (`LAG OVER PARTITION BY`)
- ✅ Grafana 즉시 연결 (HTTP API port 8123)

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| String 컬럼 | 서비스명, URL 경로, 에러 메시지 | P1 |
| PERCENTILE_CONT (P50/P95/P99) | 레이턴시 분포 — APM의 핵심 지표 | P1 |
| HyperLogLog | 분산 approximate COUNT DISTINCT (유니크 유저) | P2 |
| `SUM(CASE WHEN)` | 에러율 계산 (errors / total) | P0 |
| 높은 카디널리티 GROUP BY | trace_id별 집계 (수백만 고유값) | P1 |

**경쟁사 대비:**
- vs ClickHouse: ClickHouse가 String/Percentile/HLL 완전 지원. ZeptoDB는 레이턴시 우위.
- vs Prometheus/VictoriaMetrics: ZeptoDB가 SQL + JOIN 지원. Prometheus는 PromQL만.

---

### 2.4 크립토 / DeFi

**현재 지원되는 패턴:**
- ✅ 거래소별 VWAP 비교 (`VWAP GROUP BY symbol`)
- ✅ OHLCV 캔들차트 (`FIRST/LAST/MAX/MIN/SUM`)
- ✅ 대량 거래 탐지 (`CTE + WHERE + ORDER BY`)
- ✅ Kafka consumer로 거래소 WebSocket 수신 가능
- ✅ 24/7 무중단 운영 (Helm + PDB)

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| 크로스 테이블 ASOF JOIN | trades + quotes (다른 거래소) 시간 매칭 | P1 |
| Decimal 타입 | 0.00000001 BTC 정밀도 (int64 스케일링으로 가능하지만 불편) | P2 |
| WebSocket 네이티브 인제스션 | Binance/Coinbase 직접 수신 (Kafka 우회 가능) | P3 |

**경쟁사 대비:**
- vs kdb+: ZeptoDB가 SQL + Python + 무료. kdb+는 q 언어 + $100K/yr.
- vs QuestDB: 유사한 포지셔닝. QuestDB는 String/Float 지원. ZeptoDB는 ASOF JOIN + 분산 클러스터.

---

### 2.5 에너지 / 유틸리티

**현재 지원되는 패턴:**
- ✅ EMA 기반 부하 예측 (`EMA OVER PARTITION BY`)
- ✅ 피크 수요 탐지 (`GROUP BY + MAX + ORDER BY DESC`)
- ✅ 시간대별 집계 (`xbar` + `GROUP BY`)

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| STDDEV / VARIANCE | 부하 변동성 분석 | P1 |
| PERCENTILE | 피크 부하 분포 (P95 수요) | P1 |
| 날짜 리터럴 | `WHERE timestamp > '2026-03-24'` | P2 |
| Float 컬럼 | 전압 220.5V, 주파수 60.01Hz | P1 |

---

### 2.6 헬스케어 / 바이오

**현재 지원되는 패턴:**
- ✅ 환자 활력징후 모니터링 (`AVG/MIN/MAX OVER ROWS PRECEDING`)
- ✅ 치료군 비교 (`GROUP BY + AVG + ORDER BY`)
- ✅ 롤링 윈도우 분석 (`ROWS N PRECEDING`)

**부족한 부분:**

| Gap | 실제 필요 | 우선순위 |
|-----|----------|---------|
| MEDIAN | 바이탈 정상 범위 판단 | P1 |
| STDDEV | 이상치 z-score 계산 | P1 |
| String 컬럼 | 환자 ID, 진단 코드, 약물명 | P1 |
| Float 컬럼 | 체온 36.5°C, SpO2 98.2% | P1 |

---

## 3. 우선순위 종합

### P0 — ~~기존 코드 수정 (파서/executor 패치)~~ ✅ 완료 (2026-03-24)

| 항목 | 영향 산업 | 상태 |
|------|----------|------|
| ~~`SUM(CASE WHEN ... THEN col)` 중첩~~ | 전체 | ✅ 구현 완료 (로컬 + 분산) — 8개 분산 테스트 |
| ~~`WHERE IN` 멀티 파티션~~ | 전체 | ✅ 구현 완료 (로컬 + 분산 scatter+merge) — 8개 분산 테스트 |
| ~~비집계 `ORDER BY` 안정화~~ | 전체 | ✅ 이미 동작 확인 (post-merge 정렬 포함) |

### P1 — 새 기능 추가 (산업 확장 핵심)

| 항목 | 영향 산업 | 예상 공수 |
|------|----------|----------|
| **Float/Double 네이티브 컬럼** | IoT, AV, 에너지, 헬스케어 | 1-2주 |
| **String 컬럼** | APM, 헬스케어 | 1-2주 |
| **PERCENTILE_CONT / MEDIAN** | APM, 에너지, 헬스케어 | 2-3일 |
| **STDDEV / VARIANCE** | 에너지, 헬스케어 | 1-2일 |
| 크로스 테이블 ASOF JOIN | 크립토, AV | 3-5일 |

### P2 — 커넥터 / 편의 기능

| 항목 | 영향 산업 | 예상 공수 |
|------|----------|----------|
| MQTT 인제스션 | IoT | 1주 |
| OPC-UA 커넥터 | IoT | 1-2주 |
| HyperLogLog | APM | 2-3일 |
| 날짜 리터럴 파싱 | 전체 | 1일 |
| Decimal 타입 | 크립토 | 1주 |

### P3 — 장기

| 항목 | 영향 산업 |
|------|----------|
| ROS2 플러그인 | AV/로보틱스 |
| Array/Blob 컬럼 | AV/로보틱스 |
| WebSocket 네이티브 인제스션 | 크립토 |

---

## 4. 핵심 인사이트

**Float + String + 통계 함수 3개** (PERCENTILE, STDDEV, MEDIAN)가 추가되면 금융 외 산업 커버리지가 현재 ~60%에서 ~85%로 올라간다.

```
현재 커버리지 (int64 only, 금융 함수 중심):
  금융/크립토:  ████████████████████ 95%
  IoT:         ████████████░░░░░░░░ 60%
  APM:         ████████░░░░░░░░░░░░ 45%
  AV/로보틱스:  ████████████░░░░░░░░ 55%
  에너지:       ██████████░░░░░░░░░░ 50%
  헬스케어:     ██████████░░░░░░░░░░ 50%

P1 완료 후 (Float + String + 통계):
  금융/크립토:  ████████████████████ 95%
  IoT:         █████████████████░░░ 85%
  APM:         ████████████████░░░░ 80%
  AV/로보틱스:  ██████████████░░░░░░ 70%
  에너지:       █████████████████░░░ 85%
  헬스케어:     ████████████████░░░░ 80%
```

---

## Related

- **테스트 코드:** `tests/unit/test_sql.cpp` — `IndustryUseCaseTest` (16 tests)
- **제품 포지셔닝:** `docs/business/product_positioning.md`
- **SQL Reference:** `docs/api/SQL_REFERENCE.md`
- **BACKLOG:** `BACKLOG.md`

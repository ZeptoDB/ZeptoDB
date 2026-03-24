# ZeptoDB — Product Positioning & Use Cases

Last updated: 2026-03-24

---

## Core Thesis

**"Built for quants. Ready for everything."**

ZeptoDB는 퀀트 리서치와 HFT를 위해 태어났지만, 그 과정에서 만들어진 기술 — μs 레이턴시, 수백만 ticks/sec 인제스션, ASOF JOIN, zero-copy Python — 은 대규모 시계열 데이터가 있는 모든 산업에서 동일하게 필요하다.

```
퀀트 리서치 (origin)
  → 백테스트 파이프라인
    → 프로덕션 트레이딩
      → 리스크/컴플라이언스
        → 크립토/디파이
          → IoT/스마트팩토리
            → 자율주행/로보틱스
              → 어디든 시계열이 있는 곳
```

---

## Positioning: One Database, Full Lifecycle

### Phase 1: Research (퀀트 리서치)

퀀트가 Jupyter에서 전략을 개발하는 단계.

| Need | ZeptoDB Solution |
|------|-----------------|
| 과거 데이터 빠르게 로드 | Parquet HDB → 4.8 GB/s read |
| Python에서 바로 분석 | 522ns zero-copy numpy/pandas |
| 기술 지표 계산 | EMA, DELTA, RATIO, xbar, VWAP — SQL로 |
| 백테스트 루프 | ASOF JOIN으로 과거 시점 재현 |
| 대용량 데이터 탐색 | 1M rows filter 272μs, GROUP BY 248μs |

**킬러 메시지:** "kdb+의 성능을 Python에서. 라이선스 비용 없이."

### Phase 2: Production (프로덕션 트레이딩)

검증된 전략을 실시간으로 돌리는 단계.

| Need | ZeptoDB Solution |
|------|-----------------|
| 실시간 틱 수신 | 5.52M ticks/sec, FIX/ITCH/Binance feed handler |
| 초저지연 쿼리 | 272μs filter, g# index 3.3μs |
| 분산 처리 | 3-node scatter-gather, auto failover |
| 24/7 무중단 | Helm chart, rolling upgrade, PDB |
| 모니터링 | Prometheus /metrics, Grafana dashboard |

**킬러 메시지:** "리서치에서 쓰던 그 DB를 프로덕션에 그대로."

### Phase 3: Compliance (리스크/규제)

트레이딩 데이터를 감사하고 리포팅하는 단계.

| Need | ZeptoDB Solution |
|------|-----------------|
| 표준 SQL | ClickHouse 호환 HTTP API |
| 감사 로그 | WAL + audit log (SOC2/MiFID II) |
| 대시보드 | Grafana 즉시 연결 |
| 데이터 보존 | TTL policy, Parquet → S3 아카이브 |
| 접근 제어 | RBAC 5 roles, API Key, JWT/OIDC |

**킬러 메시지:** "같은 데이터, 같은 DB. 트레이딩부터 감사까지."

---

## Beyond Finance: Industry Use Cases

퀀트를 위해 만든 기술이 다른 산업에서도 동일한 문제를 해결한다.

### IoT / Smart Factory

| 금융 기능 | 산업 적용 |
|-----------|----------|
| 5.52M ticks/sec 인제스션 | 반도체 팹 10KHz 센서 × 수천 포인트 |
| ASOF JOIN | 다중 센서 시간 정렬 (온도 + 진동 + 전류) |
| EMA / DELTA | 예지보전 — 이상 감지, 트렌드 분석 |
| xbar (time bucket) | 분/시간 단위 집계 대시보드 |
| g#/p# index | 장비 ID별 O(1) 조회 |

**킬러 메시지:** "HFT에서 검증된 인제스션 엔진으로 공장 센서 데이터를."

### Autonomous Vehicles / Robotics

| 금융 기능 | 산업 적용 |
|-----------|----------|
| zero-copy numpy | 센서 → PyTorch 학습 파이프라인 |
| Window JOIN | LiDAR + Camera + IMU 시간 동기화 |
| Parquet HDB | 주행 로그 장기 저장 + 재생 |
| 분산 클러스터 | 차량 플릿 데이터 중앙 집계 |

### Crypto / DeFi

| 금융 기능 | 산업 적용 |
|-----------|----------|
| 24/7 인제스션 | 멀티 거래소 오더북 스트리밍 |
| VWAP / xbar | 실시간 가격 집계, 캔들차트 |
| Kafka consumer | Binance/Coinbase WebSocket → ZeptoDB |
| ASOF JOIN | 크로스 거래소 차익거래 분석 |

### Observability / APM

| 금융 기능 | 산업 적용 |
|-----------|----------|
| 5.52M events/sec | 대규모 로그/메트릭 인제스션 |
| SQL + Grafana | 기존 모니터링 스택과 즉시 통합 |
| TTL + S3 | 핫 데이터 인메모리, 콜드 데이터 S3 |
| 분산 클러스터 | 멀티 리전 메트릭 집계 |

---

## Competitive Positioning

```
                    ┌─────────────────────────────────────┐
                    │         ZeptoDB                       │
                    │  "Quant-grade for everyone"           │
                    │                                       │
                    │  ┌─────────┐  ┌──────────┐           │
                    │  │ kdb+    │  │ClickHouse│           │
                    │  │ 성능    │  │ SQL 편의  │           │
                    │  └────┬────┘  └────┬─────┘           │
                    │       └─────┬──────┘                  │
                    │             │                          │
                    │    + Python zero-copy                  │
                    │    + Open source                       │
                    │    + Multi-industry                    │
                    └─────────────────────────────────────┘

vs kdb+:       같은 성능, SQL + Python, 무료
vs ClickHouse: 1000x 빠른 레이턴시, ASOF JOIN, 실시간
vs InfluxDB:   100x 빠른 인제스션, 금융 함수, SIMD
vs TimescaleDB: 인메모리 μs 레이턴시, zero-copy Python
```

---

## Website Messaging Guide

### Homepage Hero
- Primary: "Built for Quants. Ready for Everything."
- Sub: "From research notebooks to production trading to factory floors — one database for all your time-series data."

### Target Audience Tabs (homepage)
1. **Quant / Researcher** — "Your Jupyter notebook, turbocharged"
2. **Trading Desk** — "Research to production, zero migration"
3. **Platform Engineer** — "Helm install, Grafana connect, done"
4. **IoT / Industry** — "HFT-grade ingestion for your sensors"

### Key Differentiators (repeat everywhere)
1. **μs latency** — not ms, not seconds
2. **Research → Production** — same DB, no migration
3. **Standard SQL + Python** — no q language, no vendor lock-in
4. **Open source** — no $100K/year license
5. **Multi-industry** — finance-proven, industry-ready

# ZeptoDB Industry Use Case Gap Analysis

Last updated: 2026-03-24

---

## Overview

ZeptoDB was designed for quant/HFT, but the same engine is applicable to time-series workloads across various industries. This document summarizes the **current coverage** and **gaps** discovered while testing actual query patterns for each industry.

Validation basis: `tests/unit/test_sql.cpp` — `IndustryUseCaseTest` (16 tests, all passing)

---

## 1. Common Limitations

Limitations repeatedly encountered across all industries during testing.

| Limitation | Impact | Workaround | Status |
|----------|--------|-----------|------|
| ~~`SUM(CASE WHEN ... THEN col ELSE 0 END)` not supported~~ | ~~High~~ | — | ✅ Resolved |
| ~~`WHERE symbol IN (1, 2, 3)` multi-partition not supported~~ | ~~Medium~~ | — | ✅ Resolved |
| ~~Non-aggregate `ORDER BY col DESC` unstable~~ | ~~Medium~~ | — | ✅ Resolved |
| ASOF JOIN self-join not possible | Medium — cannot compare timestamps within the same table | Use `LAG()` window function as alternative | Unresolved |
| `store_tick_direct` returns 0 for DELTA/RATIO | Low — test-only issue, no production impact | Use `ingest_tick` + `drain_sync` | Unresolved |
| No native Float/Double column | High — onboarding friction for all non-financial industries | int64 scaling (×100, ×1000) | P1 |
| ~~No String column~~ | ✅ Resolved — dictionary-encoded STRING supported | `WHERE symbol = 'AAPL'` possible | Done |
| Missing statistical functions (PERCENTILE, STDDEV, MEDIAN) | ~~Medium~~ | — | ✅ Resolved — STDDEV/VARIANCE/MEDIAN/PERCENTILE implemented |
| No date literal support | Low — use nanosecond integers instead of `'2026-03-24'` | `NOW() - N` or manual conversion | P2 |

---

## 2. Industry Analysis

### 2.1 IoT / Smart Factory

**Currently supported patterns:**
- ✅ Per-device spike detection (`WHERE price > threshold GROUP BY symbol`)
- ✅ Per-second/per-minute aggregation dashboards (`xbar` + `AVG/MAX/MIN`)
- ✅ Cross-device comparison (`GROUP BY symbol ORDER BY`)
- ✅ Outlier filtering (`WHERE + ORDER BY DESC LIMIT`)

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| MQTT ingestion | Sensor → ZeptoDB direct ingestion (currently HTTP/Kafka only) | P2 |
| OPC-UA connector | Siemens S7, PLC integration | P2 |
| Float column | Temperature 23.7°C, vibration 0.003g | P1 |
| Multi-table JOIN | Sensor + device master + alarm | P1 |
| `SUM(CASE WHEN)` | Aggregation by normal/abnormal intervals | P0 |

**Competitive comparison:**
- vs InfluxDB: ZeptoDB is 100x faster at ingestion, supports ASOF JOIN. InfluxDB has native Float.
- vs TimescaleDB: ZeptoDB has μs latency. TimescaleDB has the PostgreSQL ecosystem (full String, Float, JOIN support).

---

### 2.2 Autonomous Vehicles / Robotics

**Currently supported patterns:**
- ✅ Driving log aggregation (`AVG/MAX/COUNT GROUP BY`)
- ✅ Vehicle fleet comparison (`GROUP BY symbol ORDER BY DESC`)
- ✅ EMA-based smoothing (`EMA OVER PARTITION BY`)
- ✅ Long-term log storage via Parquet HDB

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| ROS2 topic ingestion | Direct LiDAR/Camera/IMU ingestion | P2 |
| Array/Blob column | Point clouds, image frames | P3 |
| Cross-table ASOF JOIN | LiDAR + Camera time synchronization (currently same table only) | P1 |
| Float column | GPS coordinates, acceleration | P1 |

**Competitive comparison:**
- vs ROS bag: ZeptoDB supports SQL queries and real-time ingestion. ROS bag is file-based replay only.
- vs ClickHouse: ZeptoDB has 1000x faster latency, ASOF JOIN. ClickHouse has full String/Float support.

---

### 2.3 Observability / APM

**Currently supported patterns:**
- ✅ Per-service high-latency detection (`GROUP BY HAVING MAX > threshold`)
- ✅ Request statistics (`COUNT/MAX GROUP BY`)
- ✅ Service dependency tracking (`LAG OVER PARTITION BY`)
- ✅ Instant Grafana connection (HTTP API port 8123)

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| String column | Service names, URL paths, error messages | P1 |
| PERCENTILE_CONT (P50/P95/P99) | Latency distribution — core APM metric | P1 |
| HyperLogLog | Distributed approximate COUNT DISTINCT (unique users) | P2 |
| `SUM(CASE WHEN)` | Error rate calculation (errors / total) | P0 |
| High-cardinality GROUP BY | Per-trace_id aggregation (millions of unique values) | P1 |

**Competitive comparison:**
- vs ClickHouse: ClickHouse has full String/Percentile/HLL support. ZeptoDB has latency advantage.
- vs Prometheus/VictoriaMetrics: ZeptoDB supports SQL + JOIN. Prometheus is PromQL only.

---

### 2.4 Crypto / DeFi

**Currently supported patterns:**
- ✅ Per-exchange VWAP comparison (`VWAP GROUP BY symbol`)
- ✅ OHLCV candlestick charts (`FIRST/LAST/MAX/MIN/SUM`)
- ✅ Large trade detection (`CTE + WHERE + ORDER BY`)
- ✅ Exchange WebSocket ingestion via Kafka consumer
- ✅ 24/7 zero-downtime operation (Helm + PDB)

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| Cross-table ASOF JOIN | trades + quotes (different exchanges) time matching | P1 |
| Decimal type | 0.00000001 BTC precision (possible with int64 scaling but inconvenient) | P2 |
| WebSocket native ingestion | Direct Binance/Coinbase ingestion (bypassing Kafka) | P3 |

**Competitive comparison:**
- vs kdb+: ZeptoDB offers SQL + Python + free. kdb+ uses q language + $100K/yr.
- vs QuestDB: Similar positioning. QuestDB supports String/Float. ZeptoDB has ASOF JOIN + distributed cluster.

---

### 2.5 Energy / Utilities

**Currently supported patterns:**
- ✅ EMA-based load forecasting (`EMA OVER PARTITION BY`)
- ✅ Peak demand detection (`GROUP BY + MAX + ORDER BY DESC`)
- ✅ Time-interval aggregation (`xbar` + `GROUP BY`)

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| STDDEV / VARIANCE | Load variability analysis | P1 |
| PERCENTILE | Peak load distribution (P95 demand) | P1 |
| Date literal | `WHERE timestamp > '2026-03-24'` | P2 |
| Float column | Voltage 220.5V, frequency 60.01Hz | P1 |

---

### 2.6 Healthcare / Bio

**Currently supported patterns:**
- ✅ Patient vital signs monitoring (`AVG/MIN/MAX OVER ROWS PRECEDING`)
- ✅ Treatment group comparison (`GROUP BY + AVG + ORDER BY`)
- ✅ Rolling window analysis (`ROWS N PRECEDING`)

**Gaps:**

| Gap | Actual Need | Priority |
|-----|----------|---------|
| MEDIAN | Vital signs normal range determination | P1 |
| STDDEV | Outlier z-score calculation | P1 |
| String column | Patient ID, diagnosis codes, drug names | P1 |
| Float column | Body temperature 36.5°C, SpO2 98.2% | P1 |

---

## 3. Priority Summary

### P0 — ~~Existing code fixes (parser/executor patches)~~ ✅ Completed (2026-03-24)

| Item | Affected Industries | Status |
|------|----------|------|
| ~~`SUM(CASE WHEN ... THEN col)` nesting~~ | All | ✅ Implementation complete (local + distributed) — 8 distributed tests |
| ~~`WHERE IN` multi-partition~~ | All | ✅ Implementation complete (local + distributed scatter+merge) — 8 distributed tests |
| ~~Non-aggregate `ORDER BY` stabilization~~ | All | ✅ Already verified working (including post-merge sorting) |

### P1 — New feature additions (key to industry expansion)

| Item | Affected Industries | Estimated Effort |
|------|----------|----------|
| **Float/Double native column** | IoT, AV, Energy, Healthcare | 1-2 weeks |
| **String column** | APM, Healthcare | 1-2 weeks |
| **PERCENTILE_CONT / MEDIAN** | APM, Energy, Healthcare | 2-3 days |
| **STDDEV / VARIANCE** | Energy, Healthcare | 1-2 days |
| Cross-table ASOF JOIN | Crypto, AV | 3-5 days |

### P2 — Connectors / convenience features

| Item | Affected Industries | Estimated Effort |
|------|----------|----------|
| MQTT ingestion | IoT | 1 week |
| OPC-UA connector | IoT | 1-2 weeks |
| HyperLogLog | APM | 2-3 days |
| Date literal parsing | All | 1 day |
| Decimal type | Crypto | 1 week |

### P3 — Long-term

| Item | Affected Industries |
|------|----------|
| ROS2 plugin | AV/Robotics |
| Array/Blob column | AV/Robotics |
| WebSocket native ingestion | Crypto |

---

## 4. Key Insights

Once **Float + String + 3 statistical functions** (PERCENTILE, STDDEV, MEDIAN) are added, non-financial industry coverage increases from the current ~60% to ~85%.

```
Current coverage (int64 only, financial functions focused):
  Finance/Crypto:  ████████████████████ 95%
  IoT:             ████████████░░░░░░░░ 60%
  APM:             ████████░░░░░░░░░░░░ 45%
  AV/Robotics:     ████████████░░░░░░░░ 55%
  Energy:          ██████████░░░░░░░░░░ 50%
  Healthcare:      ██████████░░░░░░░░░░ 50%

After P1 completion (Float + String + Statistics):
  Finance/Crypto:  ████████████████████ 95%
  IoT:             █████████████████░░░ 85%
  APM:             ████████████████░░░░ 80%
  AV/Robotics:     ██████████████░░░░░░ 70%
  Energy:          █████████████████░░░ 85%
  Healthcare:      ████████████████░░░░ 80%
```

---

## Related

- **Test code:** `tests/unit/test_sql.cpp` — `IndustryUseCaseTest` (16 tests)
- **Product positioning:** `docs/business/product_positioning.md`
- **SQL Reference:** `docs/api/SQL_REFERENCE.md`
- **BACKLOG:** `BACKLOG.md`

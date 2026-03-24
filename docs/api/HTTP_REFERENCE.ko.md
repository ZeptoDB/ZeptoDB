# ZeptoDB HTTP API 레퍼런스

*최종 업데이트: 2026-03-22*

HTTP 서버(포트 8123)는 **ClickHouse 호환**입니다. Grafana에서 ClickHouse 데이터 소스
플러그인으로 직접 연결할 수 있습니다.

---

## 목차

- [엔드포인트](#엔드포인트)
- [SQL 쿼리 — POST /](#sql-쿼리--post-)
- [응답 형식](#응답-형식)
- [인증](#인증)
- [/stats](#stats)
- [/metrics (Prometheus)](#metrics-prometheus)
- [에러 응답](#에러-응답)
- [역할 및 권한](#역할-및-권한)

---

## 빠른 시작 예제

### 서버 시작

```bash
# 빌드 (전체 빌드 방법은 README 참조)
cd build && ninja -j$(nproc)

# 기본 설정으로 시작 (포트 8123, 인증 없음)
./zepto_server --port 8123

# TLS + 인증 활성화
./zepto_server --port 8123 --tls-cert server.crt --tls-key server.key
```

### 첫 쿼리 실행

```bash
# 헬스 체크
curl http://localhost:8123/ping
# Ok

# 기본 집계 쿼리
curl -s -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume) AS vwap, count(*) AS n FROM trades WHERE symbol = 1'
# {"columns":["vwap","n"],"data":[[15037.2,1000]],"rows":1,"execution_time_us":52.3}
```

### 자주 사용하는 쿼리 패턴

```bash
# 5분 OHLCV 바
curl -s -X POST http://localhost:8123/ -d '
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low, last(price) AS close, sum(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000) ORDER BY bar ASC
' | python3 -m json.tool

# 심볼별 거래량
curl -s -X POST http://localhost:8123/ \
  -d 'SELECT symbol, sum(volume) AS total_vol FROM trades GROUP BY symbol ORDER BY symbol'

# 최근 10분간 거래 (최신순 100행)
curl -s -X POST http://localhost:8123/ \
  -d "SELECT price, volume, epoch_s(timestamp) AS ts FROM trades
      WHERE symbol = 1 AND timestamp > $(date +%s)000000000 - 600000000000
      ORDER BY timestamp DESC LIMIT 100"
```

### 인증과 함께 사용

```bash
# API 키 생성 (admin 역할)
./zepto_server --gen-key --role admin
# zepto_a1b2c3d4e5f6...  (64자 hex)

export APEX_KEY="zepto_a1b2c3d4e5f6..."

curl -s -X POST http://localhost:8123/ \
  -H "Authorization: Bearer $APEX_KEY" \
  -d 'SELECT count(*) FROM trades'
```

### Python 클라이언트 (zepto_py)

```python
import zepto_py as apex

db = zeptodb.connect("localhost", 8123)
df = db.query_pandas("SELECT symbol, avg(price) FROM trades GROUP BY symbol")
print(df)
```

---

## 엔드포인트

| 메서드 | 경로 | 인증 필요 | 설명 |
|--------|------|:---:|------|
| `POST` | `/` | 예 | SQL 쿼리 실행 |
| `GET` | `/ping` | 아니오 | 헬스 체크 — `"Ok\n"` 반환 |
| `GET` | `/health` | 아니오 | Kubernetes 라이브니스 프로브 |
| `GET` | `/ready` | 아니오 | Kubernetes 레디니스 프로브 |
| `GET` | `/stats` | 예 | 파이프라인 통계 (JSON) |
| `GET` | `/metrics` | 예 | Prometheus OpenMetrics |

공개 경로(`/ping`, `/health`, `/ready`)는 항상 인증에서 면제됩니다.

---

## SQL 쿼리 — POST /

SQL 문자열을 요청 본문으로 전송합니다. Content-Type은 필요하지 않습니다.

```bash
curl -X POST http://localhost:8123/ \
  -d 'SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1'
```

```bash
curl -X POST http://localhost:8123/ -d '
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS volume
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)
ORDER BY bar ASC
'
```

---

## 응답 형식

모든 응답은 JSON입니다.

### 성공

```json
{
  "columns": ["vwap(price, volume)", "count(*)"],
  "data": [[15037.2, 1000]],
  "rows": 1,
  "execution_time_us": 52.3
}
```

| 필드 | 타입 | 설명 |
|------|------|------|
| `columns` | `string[]` | SELECT 순서대로의 컬럼 이름 |
| `data` | `int64[][]` | 행 우선 결과 데이터. 모든 값은 int64. |
| `rows` | `int` | 결과 행 수 |
| `execution_time_us` | `float` | 쿼리 실행 시간 (마이크로초) |

> **참고:** 가격과 타임스탬프는 응답에서 int64입니다. 클라이언트 측에서 스케일 팩터를 적용하세요 (예: 센트→달러 변환 시 100으로 나눔).

---

## 인증

`APEX_TLS_ENABLED`가 컴파일되면 서버는 모든 비공개 경로에서 인증을 요구합니다.

### API 키

형식: `zepto_` + 64자 hex 문자 (256비트 엔트로피).

```bash
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer zepto_a1b2c3d4...64자hex" \
  -d 'SELECT count(*) FROM trades'
```

### JWT (HS256 또는 RS256)

```bash
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..." \
  -d 'SELECT count(*) FROM trades'
```

사용되는 JWT 클레임:
- `sub` — 주체 (사용자 식별자)
- `role` — APEX 역할: `admin`, `writer`, `reader`, `analyst`, `metrics`
- `exp` — 만료 타임스탬프

### 우선순위

`Authorization` 헤더가 `ey`로 시작하면 JWT 검증을 먼저 시도합니다.
JWT 실패 시 API 키 검증으로 폴백합니다.

---

## /stats

파이프라인 운영 통계를 JSON으로 반환합니다.

```bash
curl http://localhost:8123/stats -H "Authorization: Bearer zepto_..."
```

```json
{
  "ticks_ingested": 5000000,
  "ticks_stored": 4999800,
  "ticks_dropped": 200,
  "queries_executed": 12345,
  "total_rows_scanned": 50000000,
  "partitions_created": 10,
  "last_ingest_latency_ns": 181
}
```

| 필드 | 설명 |
|------|------|
| `ticks_ingested` | 링 버퍼로 푸시된 총 틱 수 |
| `ticks_stored` | 컬럼 스토어에 성공적으로 기록된 틱 수 |
| `ticks_dropped` | 링 버퍼 오버플로우로 드롭된 틱 수 |
| `queries_executed` | 총 SQL 쿼리 실행 수 |
| `total_rows_scanned` | 모든 쿼리에서 누적 스캔된 행 수 |
| `partitions_created` | 할당된 파티션 수 |
| `last_ingest_latency_ns` | 가장 최근 인제스트 레이턴시 (나노초) |

---

## /metrics (Prometheus)

Prometheus 스크래핑을 위한 OpenMetrics 텍스트를 반환합니다.

```bash
curl http://localhost:8123/metrics
```

```
# HELP zepto_ticks_ingested_total Total ticks ingested
# TYPE zepto_ticks_ingested_total counter
zepto_ticks_ingested_total 5000000

# HELP zepto_queries_executed_total Total SQL queries executed
# TYPE zepto_queries_executed_total counter
zepto_queries_executed_total 12345
```

### Grafana 설정

1. Grafana에서 ClickHouse 데이터 소스 추가
2. 호스트: `localhost`, 포트: `8123`
3. 프로토콜: HTTP (또는 TLS 사용 시 HTTPS)
4. 데이터베이스 불필요 — APEX는 SQL에서 `trades`, `quotes` 테이블 이름을 직접 사용

---

## 에러 응답

```json
{
  "columns": [],
  "data": [],
  "rows": 0,
  "execution_time_us": 0,
  "error": "Parse error: unexpected token 'FORM' at position 7"
}
```

자주 발생하는 에러:

| 에러 | 원인 |
|------|------|
| `"Parse error: ..."` | SQL 문법 오류 |
| `"Unknown table: foo"` | 테이블 없음 |
| `"Query cancelled"` | CancellationToken으로 취소됨 |
| `"Unauthorized"` | 자격증명 없음 또는 유효하지 않음 |
| `"Forbidden"` | 유효한 자격증명이지만 권한 부족 |

HTTP 상태 코드: 성공 시 `200` (SQL 오류도 200 — `error` 필드 확인), 인증 실패 시 `401`, 권한 부족 시 `403`.

---

## 역할 및 권한

| 역할 | 쿼리 | 인제스트 | Stats | Metrics | 관리자 |
|------|:-----:|:------:|:-----:|:-------:|:-----:|
| `admin` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `writer` | ✅ | ✅ | ✅ | ❌ | ❌ |
| `reader` | ✅ | ❌ | ❌ | ❌ | ❌ |
| `analyst` | ✅ | ❌ | ✅ | ✅ | ❌ |
| `metrics` | ❌ | ❌ | ✅ | ✅ | ❌ |

---

*참고: [SQL 레퍼런스](SQL_REFERENCE_ko.md) · [Python 레퍼런스](PYTHON_REFERENCE_ko.md) · [C++ 레퍼런스](CPP_REFERENCE_ko.md)*

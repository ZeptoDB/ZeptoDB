# APEX-DB API 레퍼런스

*최종 업데이트: 2026-03-22*

최상위 인덱스 문서입니다. 각 레퍼런스는 `docs/api/` 아래 별도 파일에 있습니다.

---

## 레퍼런스 문서

| 문서 | 내용 |
|------|------|
| [docs/api/SQL_REFERENCE_ko.md](api/SQL_REFERENCE_ko.md) | SQL 전체 문법 — SELECT, WHERE, 집계, 윈도우 함수, 금융 함수, 날짜/시간 함수, JOIN, 집합 연산, CASE WHEN |
| [docs/api/PYTHON_REFERENCE_ko.md](api/PYTHON_REFERENCE_ko.md) | `apex` pybind11 바인딩, `apex_py` 패키지 — connection, dataframe, arrow, streaming |
| [docs/api/CPP_REFERENCE_ko.md](api/CPP_REFERENCE_ko.md) | `ApexPipeline`, `QueryExecutor`, `PartitionManager`, `TickMessage`, `CancellationToken` |
| [docs/api/HTTP_REFERENCE_ko.md](api/HTTP_REFERENCE_ko.md) | HTTP 엔드포인트, JSON 응답 형식, 인증, Prometheus 메트릭, 역할 |

영어 원본:

| 문서 | 내용 |
|------|------|
| [docs/api/SQL_REFERENCE.md](api/SQL_REFERENCE.md) | SQL Reference (English) |
| [docs/api/PYTHON_REFERENCE.md](api/PYTHON_REFERENCE.md) | Python API Reference (English) |
| [docs/api/CPP_REFERENCE.md](api/CPP_REFERENCE.md) | C++ API Reference (English) |
| [docs/api/HTTP_REFERENCE.md](api/HTTP_REFERENCE.md) | HTTP API Reference (English) |

---

## 빠른 참조

### SQL — 자주 사용하는 패턴

```sql
-- VWAP + 카운트
SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1

-- 5분 OHLCV 바 (kdb+ xbar 스타일)
SELECT xbar(timestamp, 300000000000) AS bar,
       first(price) AS open, max(price) AS high,
       min(price) AS low,   last(price) AS close,
       sum(volume) AS vol
FROM trades WHERE symbol = 1
GROUP BY xbar(timestamp, 300000000000)

-- 이동 평균
SELECT price, AVG(price) OVER (PARTITION BY symbol ROWS 20 PRECEDING) AS ma20
FROM trades

-- 날짜 트런케이션
SELECT DATE_TRUNC('min', timestamp) AS minute, sum(volume)
FROM trades GROUP BY DATE_TRUNC('min', timestamp)
```

### Python — 자주 사용하는 패턴

```python
import apex
from apex_py import from_polars, to_pandas

# 설정
pipeline = apex.Pipeline()
pipeline.start()

# polars에서 인제스트 (제로카피 Arrow 경로)
from_polars(df, pipeline, symbol_col="sym", price_col="px", volume_col="vol")

# 제로카피 numpy
prices = pipeline.get_column(symbol=1, name="price")

# HTTP 클라이언트
db = apex.connect("localhost", 8123)
df = db.query_pandas("SELECT avg(price) FROM trades GROUP BY symbol")
```

### C++ — 자주 사용하는 패턴

```cpp
// 설정
ApexPipeline pipeline;
pipeline.start();

// 인제스트
TickMessage msg{.symbol_id=1, .price=15000, .volume=100, .recv_ts=now_ns()};
pipeline.ingest_tick(msg);

// SQL
QueryExecutor exec{pipeline};
exec.enable_parallel();
auto result = exec.execute("SELECT vwap(price, volume) FROM trades WHERE symbol = 1");

// 직접 쿼리
auto r = pipeline.query_vwap(1);
```

### HTTP — 자주 사용하는 패턴

```bash
# 쿼리
curl -X POST http://localhost:8123/ \
  -H "Authorization: Bearer apex_<키>" \
  -d 'SELECT vwap(price, volume) FROM trades WHERE symbol = 1'

# 통계
curl http://localhost:8123/stats -H "Authorization: Bearer apex_<키>"

# 헬스 (인증 불필요)
curl http://localhost:8123/ping
```

---

## 상호운용성 매트릭스

| 출발 \ 목적지 | pandas | polars | numpy | Arrow | DuckDB | HTTP |
|---------------|--------|--------|-------|-------|--------|------|
| APEX-DB (인프로세스) | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` | — |
| APEX-DB (HTTP) | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — | `POST /` |
| pandas → APEX | `from_pandas()` | — | — | — | — | `ingest_pandas()` |
| polars → APEX | — | `from_polars()` | — | — | — | `ingest_polars()` |
| Arrow → APEX | — | — | — | `ingest_arrow()` | — | — |

---

## 참고 자료

- `docs/design/layer4_transpiler_client.md` — SQL 기능 매트릭스 및 설계 결정
- `docs/devlog/014_sql_phase2_phase3.md` — SQL Phase 2/3 구현 노트
- `docs/devlog/013_python_ecosystem.md` — Python 에코시스템 구현
- `docs/design/layer5_security_auth.md` — TLS/JWT/API 키 설정
- `docs/deployment/PRODUCTION_DEPLOYMENT.md` — 프로덕션 배포 가이드

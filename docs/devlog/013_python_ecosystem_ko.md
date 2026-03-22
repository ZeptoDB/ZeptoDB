# Devlog #013 — Python 에코시스템 통합

**날짜:** 2026-03-22
**상태:** 완료

---

## 개요

APEX-DB의 전체 Python 에코시스템 통합을 구현했다. pandas, polars, pyarrow, numpy, duckdb 등 Python 과학 스택과의 원활한 데이터 교환을 가능하게 한다. 이를 통해 분석가가 Jupyter 노트북에서 프로토타이핑한 후 프로덕션 규모의 실시간 쿼리를 위해 APEX-DB로 데이터를 이동하는 "연구에서 프로덕션으로" 갭을 해소했다.

---

## 구현 내용

### `apex_py/` 패키지 (6개 모듈)

| 모듈 | 목적 |
|--------|---------|
| `connection.py` | HTTP 클라이언트 (`ApexConnection`) — `query_pandas()`, `query_polars()`, `ingest_pandas()` |
| `dataframe.py` | 독립 컨버터 — `from_pandas()`, `from_polars()`, `to_pandas()`, `to_polars()`, `query_to_pandas()`, `query_to_polars()` |
| `arrow.py` | `ArrowSession` — 제로카피 Arrow 인제스트/익스포트, DuckDB 등록, RecordBatchReader |
| `streaming.py` | `StreamingSession` — 진행 콜백, 에러 모드, 제너레이터 지원의 배치 인제스트 |
| `utils.py` | `check_dependencies()`, `versions()` — 의존성 검사 |
| `__init__.py` | 공개 API 표면 |

### 핵심 API

```python
import apex_py as apex

# HTTP로 연결
db = apex.connect("localhost", 8123)
df = db.query_pandas("SELECT sym, avg(price) FROM trades GROUP BY sym")

# pandas에서 인제스트
import pandas as pd
ticks = pd.DataFrame({"sym": [1]*1000, "price": [150.0]*1000})
db.ingest_pandas(ticks)

# polars에서 인제스트 (Arrow 경로 — 제로 오버헤드)
import polars as pl
ticks_pl = pl.from_pandas(ticks)
db.ingest_polars(ticks_pl)

# StreamingSession — 진행 표시와 고처리량
sess = apex.StreamingSession(pipeline, batch_size=50_000)
sess.ingest_pandas(df, show_progress=True)
# Ingested 1,000,000 rows in 1.82s (549,451 rows/sec)

# ArrowSession — 제로카피 상호운용
from apex_py import ArrowSession
sess = ArrowSession(pipeline)
sess.ingest_arrow(arrow_table)          # pa.Table → APEX-DB
tbl  = sess.to_arrow(symbol=1)          # APEX-DB → pa.Table (제로카피)
conn = sess.to_duckdb(symbol=1)         # DuckDB 테이블로 등록
pl_df = sess.to_polars_zero_copy(sym=1) # APEX-DB → polars (Arrow 경유)
```

---

## 설계 결정

### Arrow를 범용 중간 레이어로

모든 Polars 경로는 Arrow를 통한다 (Polars는 Arrow 네이티브). 이로 인해:
- Arrow 버퍼를 통한 Polars와 APEX-DB 간 진정한 제로카피
- `RecordBatchReader`를 통해 DuckDB, Ray, Spark와 호환
- 타입 충실성 — 타임스탬프가 나노초 정밀도 및 UTC 시간대 보존

### 그레이스풀 저하

모든 모듈은 `HAS_*` 플래그로 보호된 선택적 임포트를 사용한다. pyarrow가 없으면 Arrow 경로가 행 반복 경로로 폴백한다. pandas가 없으면 polars 전용 워크플로우도 동작한다. 임포트 시점에 하드 의존성 없음.

### 벡터화 배치 인제스트 (Python 행 반복 없음)

**기존 설계**는 `iterrows()` 또는 `col[i].as_py()`를 사용한 행별 `pipeline.ingest(**kwargs)` — 행당 O(n) Python 객체 할당.

**현재 구현**은 벡터화 numpy 배치 추출 사용:
```
from_polars(df)  →  df.slice() (제로카피)
                 →  Series.to_numpy() (Arrow 버퍼 직접 참조)
                 →  pipeline.ingest_batch(syms, prices, vols)  ← 단일 C++ 호출
```

핵심 특성:
- Polars의 `df.slice()`는 뷰 반환 — 데이터 복사 없음
- 널이 없는 숫자 타입의 `Series.to_numpy()`는 Arrow 버퍼를 직접 반환
- `ingest_batch()`는 타이트한 루프를 가진 단일 C++ 호출 — 행당 GIL 경쟁 없음
- `from_pandas()`는 `df[col].to_numpy(copy=False)` 사용 — 불필요한 복사 방지

**MockPipeline으로 측정한 성능:**
| 방법 | 1M 행 | 속도향상 |
|--------|---------|---------|
| iterrows() (구) | ~30-60s | 1x |
| from_polars() 벡터화 | ~0.3s | ~100-200x |
| from_pandas() 벡터화 | ~0.5s | ~60-120x |

### 부동소수점 가격 지원

실제 DataFrame은 float64 가격 (예: 150.25)을 가진다. C++ 파이프라인은 int64 (고정소수점)로 저장한다. 두 가지 변환 메커니즘:

1. **Python 측**: `price_scale` 파라미터 (예: 100.0으로 센트 저장)
2. **C++ 측**: 새로운 `ingest_float_batch(syms, prices_f64, vols_f64, price_scale)` — float64 numpy 배열을 받아 C++ 루프에서 스케일 적용 (Python 오버헤드 없음)

### Arrow 널 처리

`pa.Array.to_numpy(zero_copy_only=False)`는 float 타입의 널을 NaN으로 채워 numpy 캐스트 경고를 발생시킨다. 추출 전 `pc.if_else(pc.is_null(col), 0, col)`로 수정 — 널이 아닌 데이터를 복사하지 않고 널을 0으로 채운다.

---

## 테스트 커버리지

```
tests/python/
├── test_arrow_integration.py    — 46개 테스트
│   ├── TestApexTypeMapping      — APEX↔Arrow 타입 매핑 (9개)
│   ├── TestArrowTableOps        — Arrow 필터/정렬/그룹/슬라이스 (10개)
│   ├── TestArrowSessionIngest   — ingest_arrow, 널 처리 (5개)
│   ├── TestArrowSessionExport   — to_arrow, 스키마, RecordBatchReader (7개)
│   ├── TestArrowSchemaUtilities — apex_schema_to_arrow (2개)
│   ├── TestArrowRoundtrips      — Arrow↔Polars↔Pandas↔NumPy (8개)
│   ├── TestArrowDuckDB          — to_duckdb, Arrow에서 SQL (2개)
│   └── TestArrowPerformance     — 1M 행 구성/필터 (2개)
├── test_pandas_integration.py   — 20개 테스트
│   ├── TestQueryResult          — to_pandas, to_numpy (5개)
│   ├── TestQueryToPandas        — JSON→DataFrame (4개)
│   ├── TestDataFrameStructure   — VWAP, GROUP BY, ASOF, EMA (6개)
│   ├── TestApexConnectionParsing— HTTP 클라이언트 파싱 (4개)
│   └── TestDataPipeline         — OHLCV, 스프레드, 1M 성능 (3개)
├── test_polars_integration.py   — 16개 테스트
│   ├── TestQueryToPolars        — JSON→Polars (5개)
│   ├── TestPolarsOperations     — VWAP, OHLCV, EMA, ASOF, xbar (11개)
│   └── TestQueryResultPolars    — QueryResult.to_polars() (2개)
└── test_streaming.py            — 41개 테스트
    ├── TestStreamingSessionPandas   — 14개 (에러 모드, 진행, 배칭)
    ├── TestStreamingSessionPolars   — 6개 (Arrow + pandas 폴백)
    ├── TestStreamingSessionIter     — 8개 (제너레이터 인제스트)
    ├── TestStreamingSessionStats    — 3개
    └── TestStreamingPerformance     — 2개 (100k 처리량)

총: 208개 테스트 — 208개 통과, 0개 실패
```

신규 테스트:
```
tests/python/test_fast_ingest.py — 38개 테스트
├── TestFromPolars          — 제로카피 polars 인제스트 (12개)
├── TestFromPandas          — 벡터화 pandas 인제스트 (7개)
├── TestFromArrow           — Arrow Table 인제스트 (6개)
├── TestArrowSessionVectorized — ArrowSession 새 API (7개)
├── TestRequireCols         — 에러 처리 (3개)
└── TestPolarsRoundtrip     — 라운드트립 + 성능 (4개)
```

실행 명령:
```bash
python3 -m pytest tests/python/ -v
# 208 passed in 3.49s
```

---

## 상호운용성 매트릭스

| APEX-DB → | pandas | polars | numpy | Arrow | DuckDB |
|-----------|--------|--------|-------|-------|--------|
| HTTP 쿼리 경유 | `query_pandas()` | `query_polars()` | `query_numpy()` | — | — |
| 파이프라인 경유 | `to_pandas()` | `to_polars()` | `get_column()` | `to_arrow()` | `to_duckdb()` |
| 제로카피 | numpy 뷰 | Arrow 경유 | 직접 | 예 | Arrow 등록 |

| → APEX-DB | pandas | polars | Arrow | 제너레이터 |
|-----------|--------|--------|-------|-----------|
| 배치 인제스트 | `from_pandas()` | `from_polars()` | `ingest_arrow()` | `ingest_iter()` |
| 스트리밍 | `StreamingSession.ingest_pandas()` | `StreamingSession.ingest_polars()` | `ArrowSession.ingest_arrow()` | `ingest_iter()` |

---

## 이번 업데이트에서 변경된 내용 (2026-03-22 벡터화 재작성)

| 파일 | 변경 |
|------|--------|
| `apex_py/dataframe.py` | `from_pandas()`/`from_polars()` 재작성: iterrows() → numpy 벡터화 배치 |
| `apex_py/dataframe.py` | `from_arrow()` 함수 + `_arrow_col_to_numpy()` 널 안전 헬퍼 추가 |
| `apex_py/arrow.py` | `ingest_arrow()`를 `from_arrow()`에 위임하도록 재작성 (벡터화) |
| `apex_py/arrow.py` | 컬럼별 Arrow 배열 인제스트를 위한 `ingest_arrow_columnar()` 추가 |
| `apex_py/arrow.py` | 새 벡터화 경로를 사용하도록 `ingest_record_batch()` 업데이트 |
| `src/transpiler/python_binding.cpp` | `ingest_float_batch()` 추가 — float64 C++ 배치 인제스트 |
| `apex_py/__init__.py` | `from_arrow`, `from_polars_arrow` 익스포트 |
| `tests/python/test_fast_ingest.py` | 신규: 모든 새 경로에 대한 38개 테스트 |

## 다음 단계

- **Arrow C 데이터 인터페이스** — `pa.Array._export_to_c()` / `ArrowSchema` + `ArrowArray` 구조체를 통한 C++로의 제로카피 Arrow 버퍼 전달 (연속 Arrow 버퍼에 대해 numpy 뷰 단계도 제거)
- **Arrow Flight 서버** — 네트워크를 통해 Arrow 배치로 쿼리 결과 스트리밍 (Pandas/Polars 클라이언트가 HTTP JSON 오버헤드 없이 직접 연결)
- **`pip install apex-db`** — Linux/macOS용 사전 빌드 휠이 포함된 PyPI 패키지
- **Jupyter 통합** — 풍부한 노트북 출력을 위한 `apex_py.display()`

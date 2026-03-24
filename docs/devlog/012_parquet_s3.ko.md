# devlog #012 — HDB Parquet 저장 + S3 업로드

**날짜**: 2026-03-22
**작업**: HDB 저장 형식 확장 — Apache Parquet, AWS S3

---

## 배경 및 목표

기존 HDB는 ZeptoDB 전용 바이너리 포맷(`.bin`)만 지원했다.
이 포맷은 최고속 로컬 I/O에 최적화되어 있지만, 외부 도구(DuckDB, Spark, Polars)와
상호운용이 불가능하다는 단점이 있었다.

**추가 목표:**
1. **Apache Parquet 저장** — Arrow 에코시스템 완전 호환
2. **S3 업로드** — 클라우드 스토리지 자동 연동, 재해 복구
3. **두 형식 동시 저장(BOTH)** — 로컬 고속 쿼리 + 외부 상호운용 병행

---

## 설계: 저장 경로 확장

```
Partition (SEALED)
    │
    ├──[BINARY]───► HDBWriter → {base}/{symbol}/{hour}/{col}.bin
    │                            (기존, LZ4 압축, 최고속)
    │
    ├──[PARQUET]──► ParquetWriter → {base}/{symbol}/{hour}/{symbol}_{hour}.parquet
    │                               (Arrow 호환, SNAPPY/ZSTD/LZ4 압축)
    │                    │
    │                    └──[S3]──► S3Sink → s3://{bucket}/{prefix}/{symbol}/{hour}.parquet
    │
    └──[BOTH]────► BINARY + PARQUET 동시 저장
```

---

## 구현 파일

| 파일 | 역할 |
|------|------|
| `include/zeptodb/storage/parquet_writer.h` | ParquetWriter 헤더 (Arrow/Parquet API) |
| `src/storage/parquet_writer.cpp` | Parquet 직렬화 구현 |
| `include/zeptodb/storage/s3_sink.h` | S3Sink 헤더 (AWS SDK C++) |
| `src/storage/s3_sink.cpp` | S3 업로드 구현 |
| `include/zeptodb/storage/flush_manager.h` | FlushConfig에 출력 형식 옵션 추가 |
| `src/storage/flush_manager.cpp` | flush_partition_parquet() 통합 |
| `CMakeLists.txt` | Arrow/Parquet/AWS SDK 의존성 추가 |

---

## ParquetWriter 상세

### ColumnType → Arrow DataType 매핑

| ZeptoDB ColumnType | Arrow DataType | Parquet 물리 타입 |
|--------------------|----------------|-------------------|
| INT32              | int32()        | INT32 |
| INT64              | int64()        | INT64 |
| FLOAT32            | float32()      | FLOAT |
| FLOAT64            | float64()      | DOUBLE |
| TIMESTAMP_NS       | timestamp(ns, UTC) | INT64 (TIMESTAMP 논리 타입) |
| SYMBOL             | uint32()       | INT32 |
| BOOL               | boolean()      | BOOLEAN |

### 지원 압축 코덱

| 코덱 | 속도 | 압축률 | 추천 용도 |
|------|------|--------|-----------|
| SNAPPY (기본) | ★★★★★ | ★★★ | 실시간 플러시 |
| ZSTD | ★★★ | ★★★★★ | 콜드 스토리지 / S3 장기 보관 |
| LZ4_RAW | ★★★★★ | ★★★ | 최고속 압축 |
| NONE | — | — | 테스트 / 디버깅 |

### flush_to_buffer(): in-memory 직렬화

S3 업로드를 로컬 파일 없이 직접 수행할 때 사용:
```cpp
auto buf = parquet_writer.flush_to_buffer(partition);
s3_sink.upload_buffer(
    reinterpret_cast<const char*>(buf->data()),
    buf->size(),
    s3_key);
```

---

## S3Sink 상세

### 파티션 S3 경로 규칙

```
s3://{bucket}/{prefix}/{symbol_id}/{hour_epoch}.parquet

예시:
  s3://zepto-hdb/prod/hdb/1/1742648000.parquet
  s3://zepto-hdb/prod/hdb/2/1742648000.parquet
```

### AWS 자격증명 자동 탐지

AWS SDK 표준 credential chain 사용:
1. 환경 변수 (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`)
2. `~/.aws/credentials` 파일
3. IAM Role (EC2 Instance Profile) — 프로덕션 권장

### MinIO (self-hosted S3) 설정

```cpp
S3SinkConfig config;
config.bucket        = "zepto-hdb";
config.endpoint_url  = "http://minio:9000";
config.use_path_style = true;   // MinIO 필수
```

---

## FlushConfig 사용 예시

### 1. 기존 Binary 전용 (변경 없음)
```cpp
FlushConfig config;
config.output_format = HDBOutputFormat::BINARY;  // 기본값
// → 기존 동작 그대로
```

### 2. Parquet 전용 (DuckDB/Polars 상호운용)
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::PARQUET;
config.parquet_config.compression  = ParquetCompression::SNAPPY;
```

### 3. Parquet + S3 자동 업로드
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::PARQUET;
config.parquet_config.compression  = ParquetCompression::ZSTD;

config.enable_s3_upload            = true;
config.s3_config.bucket            = "zepto-hdb-prod";
config.s3_config.prefix            = "hdb";
config.s3_config.region            = "ap-southeast-1";  // 싱가포르
config.delete_local_after_s3       = true;  // 로컬 스토리지 절약

FlushManager flush_mgr(pm, hdb_writer, config);
flush_mgr.start();
// → 파티션 봉인 시 자동으로 Parquet 저장 후 S3 업로드
```

### 4. BOTH 모드 (로컬 Binary + S3 Parquet 병행)
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::BOTH;
config.enable_s3_upload            = true;
config.s3_config.bucket            = "zepto-hdb-backup";
// → Binary는 로컬 NVMe에, Parquet는 S3에 보관
```

---

## DuckDB에서 S3 Parquet 직접 쿼리

```sql
-- S3에 저장된 ZeptoDB HDB를 DuckDB로 직접 쿼리
INSTALL httpfs;
LOAD httpfs;
SET s3_region='ap-southeast-1';

-- 특정 심볼 하루치 5분봉 조회
SELECT
    epoch_ms(timestamp / 1000000) AS bar_time,
    first(price)  AS open,
    max(price)    AS high,
    min(price)    AS low,
    last(price)   AS close,
    sum(volume)   AS volume
FROM read_parquet('s3://zepto-hdb-prod/hdb/1/*.parquet')
GROUP BY time_bucket(INTERVAL '5 minutes', bar_time)
ORDER BY bar_time;
```

---

## Polars에서 직접 읽기

```python
import polars as pl

# S3 Parquet → Polars DataFrame
df = pl.read_parquet("s3://zepto-hdb-prod/hdb/1/1742648000.parquet")

# EMA 계산 (ZeptoDB 없이 Polars로)
df = df.with_columns([
    pl.col("price").ewm_mean(span=20).alias("ema20")
])
```

---

## 의존성 설치

```bash
# Amazon Linux 2023
sudo dnf install -y arrow-devel parquet-devel    # Arrow + Parquet
sudo dnf install -y aws-sdk-cpp-s3              # AWS SDK S3

# Ubuntu 22.04+
sudo apt install -y libarrow-dev libparquet-dev
sudo apt install -y libaws-sdk-cpp-s3

# 빌드 (Parquet + S3 활성화)
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPEX_USE_PARQUET=ON \
  -DAPEX_USE_S3=ON
ninja -j$(nproc)
```

---

## 다음 단계

- [ ] Parquet 읽기 (`HDBReader` Parquet 지원 — `read_parquet()`)
- [ ] S3 파티션 목록 조회 (`list_partitions_s3()`)
- [ ] S3에서 직접 읽기 (S3 → Arrow → 쿼리, 로컬 캐시 없이)
- [ ] Parquet 메타데이터 활용 (통계 기반 파티션 프루닝)
- [ ] 멀티파트 업로드 (64MB 이상 파티션 자동 전환)

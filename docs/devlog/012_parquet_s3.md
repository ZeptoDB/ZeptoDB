# devlog #012 — HDB Parquet Storage + S3 Upload

**Date:** 2026-03-22
**Work:** HDB storage format extension — Apache Parquet, AWS S3

---

## Background and Goals

The existing HDB only supported APEX-DB's proprietary binary format (`.bin`).
This format is optimized for maximum-speed local I/O, but has the disadvantage of being
incompatible with external tools (DuckDB, Spark, Polars).

**Additional Goals:**
1. **Apache Parquet storage** — full Arrow ecosystem compatibility
2. **S3 upload** — automatic cloud storage sync, disaster recovery
3. **Dual format storage (BOTH)** — local high-speed queries + external interoperability in parallel

---

## Design: Storage Path Extension

```
Partition (SEALED)
    |
    ├──[BINARY]───> HDBWriter -> {base}/{symbol}/{hour}/{col}.bin
    |                            (existing, LZ4 compressed, fastest)
    |
    ├──[PARQUET]──> ParquetWriter -> {base}/{symbol}/{hour}/{symbol}_{hour}.parquet
    |                                (Arrow compatible, SNAPPY/ZSTD/LZ4 compressed)
    |                    |
    |                    └──[S3]──> S3Sink -> s3://{bucket}/{prefix}/{symbol}/{hour}.parquet
    |
    └──[BOTH]────> BINARY + PARQUET stored simultaneously
```

---

## Implementation Files

| File | Role |
|------|------|
| `include/apex/storage/parquet_writer.h` | ParquetWriter header (Arrow/Parquet API) |
| `src/storage/parquet_writer.cpp` | Parquet serialization implementation |
| `include/apex/storage/s3_sink.h` | S3Sink header (AWS SDK C++) |
| `src/storage/s3_sink.cpp` | S3 upload implementation |
| `include/apex/storage/flush_manager.h` | FlushConfig output format options added |
| `src/storage/flush_manager.cpp` | flush_partition_parquet() integration |
| `CMakeLists.txt` | Arrow/Parquet/AWS SDK dependency additions |

---

## ParquetWriter Details

### ColumnType to Arrow DataType Mapping

| APEX-DB ColumnType | Arrow DataType | Parquet Physical Type |
|--------------------|----------------|----------------------|
| INT32 | int32() | INT32 |
| INT64 | int64() | INT64 |
| FLOAT32 | float32() | FLOAT |
| FLOAT64 | float64() | DOUBLE |
| TIMESTAMP_NS | timestamp(ns, UTC) | INT64 (TIMESTAMP logical type) |
| SYMBOL | uint32() | INT32 |
| BOOL | boolean() | BOOLEAN |

### Supported Compression Codecs

| Codec | Speed | Compression | Recommended Use |
|-------|-------|-------------|-----------------|
| SNAPPY (default) | ***** | *** | Real-time flush |
| ZSTD | *** | ***** | Cold storage / S3 long-term |
| LZ4_RAW | ***** | *** | Maximum-speed compression |
| NONE | — | — | Testing / debugging |

### flush_to_buffer(): In-Memory Serialization

Used for direct S3 upload without a local file:
```cpp
auto buf = parquet_writer.flush_to_buffer(partition);
s3_sink.upload_buffer(
    reinterpret_cast<const char*>(buf->data()),
    buf->size(),
    s3_key);
```

---

## S3Sink Details

### Partition S3 Path Convention

```
s3://{bucket}/{prefix}/{symbol_id}/{hour_epoch}.parquet

Examples:
  s3://apex-hdb/prod/hdb/1/1742648000.parquet
  s3://apex-hdb/prod/hdb/2/1742648000.parquet
```

### AWS Credential Auto-Detection

Uses AWS SDK standard credential chain:
1. Environment variables (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`)
2. `~/.aws/credentials` file
3. IAM Role (EC2 Instance Profile) — recommended for production

### MinIO (self-hosted S3) Configuration

```cpp
S3SinkConfig config;
config.bucket        = "apex-hdb";
config.endpoint_url  = "http://minio:9000";
config.use_path_style = true;   // required for MinIO
```

---

## FlushConfig Usage Examples

### 1. Existing Binary Only (no changes)
```cpp
FlushConfig config;
config.output_format = HDBOutputFormat::BINARY;  // default
// -> same behavior as before
```

### 2. Parquet Only (DuckDB/Polars interoperability)
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::PARQUET;
config.parquet_config.compression  = ParquetCompression::SNAPPY;
```

### 3. Parquet + S3 Auto-Upload
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::PARQUET;
config.parquet_config.compression  = ParquetCompression::ZSTD;

config.enable_s3_upload            = true;
config.s3_config.bucket            = "apex-hdb-prod";
config.s3_config.prefix            = "hdb";
config.s3_config.region            = "ap-southeast-1";  // Singapore
config.delete_local_after_s3       = true;  // save local storage

FlushManager flush_mgr(pm, hdb_writer, config);
flush_mgr.start();
// -> automatically saves Parquet and uploads to S3 when partition is sealed
```

### 4. BOTH Mode (local Binary + S3 Parquet in parallel)
```cpp
FlushConfig config;
config.output_format               = HDBOutputFormat::BOTH;
config.enable_s3_upload            = true;
config.s3_config.bucket            = "apex-hdb-backup";
// -> Binary stored on local NVMe, Parquet stored on S3
```

---

## Direct S3 Parquet Query from DuckDB

```sql
-- Query APEX-DB HDB stored in S3 directly with DuckDB
INSTALL httpfs;
LOAD httpfs;
SET s3_region='ap-southeast-1';

-- Query 5-minute bars for one symbol for one day
SELECT
    epoch_ms(timestamp / 1000000) AS bar_time,
    first(price)  AS open,
    max(price)    AS high,
    min(price)    AS low,
    last(price)   AS close,
    sum(volume)   AS volume
FROM read_parquet('s3://apex-hdb-prod/hdb/1/*.parquet')
GROUP BY time_bucket(INTERVAL '5 minutes', bar_time)
ORDER BY bar_time;
```

---

## Direct Read from Polars

```python
import polars as pl

# S3 Parquet -> Polars DataFrame
df = pl.read_parquet("s3://apex-hdb-prod/hdb/1/1742648000.parquet")

# EMA calculation (with Polars, without APEX-DB)
df = df.with_columns([
    pl.col("price").ewm_mean(span=20).alias("ema20")
])
```

---

## Dependency Installation

```bash
# Amazon Linux 2023
sudo dnf install -y arrow-devel parquet-devel    # Arrow + Parquet
sudo dnf install -y aws-sdk-cpp-s3              # AWS SDK S3

# Ubuntu 22.04+
sudo apt install -y libarrow-dev libparquet-dev
sudo apt install -y libaws-sdk-cpp-s3

# Build (Parquet + S3 enabled)
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPEX_USE_PARQUET=ON \
  -DAPEX_USE_S3=ON
ninja -j$(nproc)
```

---

## Next Steps

- [ ] Parquet read (`HDBReader` Parquet support — `read_parquet()`)
- [ ] S3 partition listing (`list_partitions_s3()`)
- [ ] Direct S3 read (S3 -> Arrow -> query, without local cache)
- [ ] Parquet metadata utilization (statistics-based partition pruning)
- [ ] Multipart upload (auto-switch for partitions > 64MB)

# Parquet + S3 Activation Guide

## Current Status (2026-03-26)

- `ZEPTO_USE_PARQUET=ON` ✅ Enabled (pyarrow 21.0.0 bundled C++ library)
- `ZEPTO_USE_S3=ON` ✅ Enabled (AWS SDK C++ 1.11.510, installed in /usr/local)

## Installed Libraries

### Arrow + Parquet
- **Source**: pyarrow 21.0.0 (pip3 install pyarrow)
- **Libraries**: `~/.local/lib/python3.9/site-packages/pyarrow/libarrow.so.2100`, `libparquet.so.2100`
- **Headers**: `~/.local/lib/python3.9/site-packages/pyarrow/include/`
- **ldconfig**: `/etc/ld.so.conf.d/pyarrow.conf` registration complete
- **CMake detection**: pyarrow fallback (IMPORTED targets `PyArrow::arrow`, `PyArrow::parquet`)

### AWS SDK C++ (S3)
- **Version**: 1.11.510
- **Build**: Source build (static, S3 only)
- **Install path**: `/usr/local/lib64/`, `/usr/local/include/aws/`
- **Dependencies**: libcurl-devel, openssl-devel
- **CMake detection**: `find_package(AWSSDK CONFIG COMPONENTS s3)`

## Reinstallation Instructions

```bash
# 1. Arrow/Parquet (pyarrow)
pip3 install pyarrow
echo "$(python3 -c 'import pyarrow; print(pyarrow.get_library_dirs()[0])')" | sudo tee /etc/ld.so.conf.d/pyarrow.conf
sudo ldconfig

# 2. AWS SDK C++ (S3)
sudo dnf install -y libcurl-devel
git clone --depth 1 --recurse-submodules --branch 1.11.510 \
  https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp
mkdir /tmp/aws-sdk-cpp-build && cd /tmp/aws-sdk-cpp-build
cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_ONLY="s3" -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF
make -j$(nproc) && sudo make install
```

## Compile Definitions

| Definition | Value | Effect |
|------|-----|------|
| `ZEPTO_PARQUET_ENABLED` | 1 | Enable FlushManager Parquet output path |
| `ZEPTO_PARQUET_AVAILABLE` | 1 | Compile ParquetWriter Arrow API code |
| `ZEPTO_S3_ENABLED` | 1 | Enable FlushManager S3 upload path |
| `ZEPTO_S3_AVAILABLE` | 1 | Compile S3Sink AWS SDK code |

## Runtime Configuration (FlushConfig)

```cpp
FlushConfig config;
config.output_format      = HDBOutputFormat::PARQUET;  // or BOTH
config.enable_s3_upload   = true;
config.s3_config.bucket   = "zeptodb-hdb";
config.s3_config.region   = "us-east-1";
config.delete_local_after_s3 = true;

// Storage Tiering
config.tiering.enabled       = true;
config.tiering.warm_after_ns = 3'600'000'000'000;       // 1h
config.tiering.cold_after_ns = 86'400'000'000'000;      // 24h → S3
config.tiering.drop_after_ns = 2'592'000'000'000'000;   // 30d → delete
```

## Related Source Files

| File | Role |
|------|------|
| `include/zeptodb/storage/parquet_writer.h` | Partition → Parquet conversion |
| `include/zeptodb/storage/parquet_reader.h` | Parquet → ColumnVector reading |
| `include/zeptodb/storage/s3_sink.h` | S3 PutObject/Multipart upload |
| `include/zeptodb/storage/flush_manager.h` | Background flush + tiering orchestration |
| `src/storage/flush_manager.cpp` | flush_partition_parquet() — Parquet save + S3 upload |

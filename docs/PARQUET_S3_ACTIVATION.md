# Parquet + S3 활성화 가이드

## 현재 상태 (2026-03-26)

- `ZEPTO_USE_PARQUET=ON` ✅ 활성화 (pyarrow 21.0.0 번들 C++ 라이브러리)
- `ZEPTO_USE_S3=ON` ✅ 활성화 (AWS SDK C++ 1.11.510, /usr/local 설치)

## 설치된 라이브러리

### Arrow + Parquet
- **소스**: pyarrow 21.0.0 (pip3 install pyarrow)
- **라이브러리**: `~/.local/lib/python3.9/site-packages/pyarrow/libarrow.so.2100`, `libparquet.so.2100`
- **헤더**: `~/.local/lib/python3.9/site-packages/pyarrow/include/`
- **ldconfig**: `/etc/ld.so.conf.d/pyarrow.conf` 등록 완료
- **CMake 탐지**: pyarrow fallback (IMPORTED 타겟 `PyArrow::arrow`, `PyArrow::parquet`)

### AWS SDK C++ (S3)
- **버전**: 1.11.510
- **빌드**: 소스 빌드 (static, S3 only)
- **설치 경로**: `/usr/local/lib64/`, `/usr/local/include/aws/`
- **의존성**: libcurl-devel, openssl-devel
- **CMake 탐지**: `find_package(AWSSDK CONFIG COMPONENTS s3)`

## 재설치 방법

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

## 컴파일 정의

| 정의 | 값 | 효과 |
|------|-----|------|
| `ZEPTO_PARQUET_ENABLED` | 1 | FlushManager Parquet 출력 경로 활성화 |
| `ZEPTO_PARQUET_AVAILABLE` | 1 | ParquetWriter Arrow API 코드 컴파일 |
| `ZEPTO_S3_ENABLED` | 1 | FlushManager S3 업로드 경로 활성화 |
| `ZEPTO_S3_AVAILABLE` | 1 | S3Sink AWS SDK 코드 컴파일 |

## 런타임 설정 (FlushConfig)

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
config.tiering.drop_after_ns = 2'592'000'000'000'000;   // 30d → 삭제
```

## 관련 소스 파일

| 파일 | 역할 |
|------|------|
| `include/zeptodb/storage/parquet_writer.h` | Partition → Parquet 변환 |
| `include/zeptodb/storage/parquet_reader.h` | Parquet → ColumnVector 읽기 |
| `include/zeptodb/storage/s3_sink.h` | S3 PutObject/Multipart 업로드 |
| `include/zeptodb/storage/flush_manager.h` | 백그라운드 flush + tiering 오케스트레이션 |
| `src/storage/flush_manager.cpp` | flush_partition_parquet() — Parquet 저장 + S3 업로드 |

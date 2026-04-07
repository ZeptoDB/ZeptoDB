# Installation

## Option 1: Docker (recommended for getting started)

```bash
docker pull zeptodb/zeptodb:0.0.1
docker run -p 8123:8123 zeptodb/zeptodb:0.0.1
```

Open [http://localhost:8123/ui/](http://localhost:8123/ui/) for the Web UI.

All features included: SIMD, JIT, TLS, S3, Arrow Flight, Parquet, Web UI.
For Docker-specific options, see [Docker Deployment Guide](../deployment/DOCKER.md).

## Option 2: Pre-built Binaries

Download from [GitHub Releases](https://github.com/ZeptoDB/ZeptoDB/releases):

```bash
# x86_64
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-amd64-0.0.1.tar.gz
tar xzf zeptodb-linux-amd64-0.0.1.tar.gz

# arm64 (Graviton, Apple Silicon Linux VMs)
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-arm64-0.0.1.tar.gz
tar xzf zeptodb-linux-arm64-0.0.1.tar.gz

# Run
./zeptodb-linux-*/zepto_http_server --port 8123 --no-auth
```

## Option 3: Build from Source (bare-metal / maximum performance)

### System Requirements

- **OS**: Amazon Linux 2023 / Fedora / Ubuntu 22.04+ / Debian bookworm
- **Compiler**: Clang 19+ (C++20)
- **Architecture**: x86_64 or aarch64 (ARM Graviton)

### Dependencies

=== "Amazon Linux 2023 / Fedora"

    ```bash
    sudo dnf install -y clang19 clang19-devel llvm19-devel \
      highway-devel numactl-devel ucx-devel ninja-build lz4-devel \
      openssl-devel libcurl-devel
    # Arrow (optional)
    sudo dnf install -y https://packages.apache.org/artifactory/arrow/amazon-linux/$(cut -d: -f6 /etc/system-release-cpe)/apache-arrow-release-latest.rpm
    sudo dnf install -y arrow-devel arrow-flight-devel parquet-devel
    ```

=== "Ubuntu / Debian"

    ```bash
    sudo apt install -y clang-19 llvm-19-dev libhwy-dev \
      libnuma-dev libucx-dev ninja-build liblz4-dev \
      libssl-dev libcurl4-openssl-dev
    # Arrow (optional)
    wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release -cs).deb
    sudo apt install -y ./apache-arrow-apt-source-latest-$(lsb_release -cs).deb
    sudo apt update && sudo apt install -y libarrow-dev libarrow-flight-dev libparquet-dev
    ```

### Build

```bash
git clone https://github.com/ZeptoDB/ZeptoDB.git && cd ZeptoDB
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)

./zepto_http_server --port 8123 --no-auth
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ZEPTO_USE_JIT` | ON | LLVM JIT query compilation |
| `ZEPTO_USE_HIGHWAY` | ON | Highway SIMD vectorization |
| `ZEPTO_USE_UCX` | ON | UCX / RDMA transport |
| `ZEPTO_USE_S3` | ON | AWS S3 upload (requires aws-sdk-cpp) |
| `ZEPTO_USE_PARQUET` | ON | Parquet file output (requires libarrow) |
| `ZEPTO_USE_FLIGHT` | ON | Arrow Flight gRPC server (requires libarrow-flight) |
| `ZEPTO_USE_LZ4` | ON | LZ4 compression |
| `ZEPTO_USE_KAFKA` | OFF | Kafka consumer (requires librdkafka) |
| `ZEPTO_USE_TCMALLOC` | OFF | tcmalloc for HFT allocation |
| `ZEPTO_USE_LTO` | OFF | Link-Time Optimization |
| `ZEPTO_BUILD_PYTHON` | ON | Python binding (pybind11) |

## Helm (Kubernetes)

```bash
helm install zeptodb ./deploy/helm/zeptodb
```

For production deployment details, see [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md).
For bare-metal tuning, see [Bare Metal Tuning](../deployment/BARE_METAL_TUNING.md).

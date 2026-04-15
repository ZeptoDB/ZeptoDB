# Binary Installation Guide

Install ZeptoDB from prebuilt binaries on GitHub Releases. No build toolchain required.

---

## Supported Platforms

| Architecture | OS | Minimum Kernel | Notes |
|---|---|---|---|
| x86_64 (amd64) | Linux | 5.10+ | Requires AVX2 (Haswell 2013+) |
| aarch64 (arm64) | Linux | 5.10+ | AWS Graviton 2/3/4 |

---

## Prerequisites

The prebuilt binaries are dynamically linked. Install the required runtime libraries for your distribution.

### Ubuntu / Debian

```bash
# Add LLVM 19 repo
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/llvm.asc
echo "deb http://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-19 main" | \
  sudo tee /etc/apt/sources.list.d/llvm.list

# Add Apache Arrow repo
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release -cs).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release -cs).deb
rm -f apache-arrow-apt-source-latest-$(lsb_release -cs).deb

# Install runtime libraries
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  llvm-19 \
  libnuma1 \
  liblz4-1 \
  liburing2 \
  libssl3 \
  libcurl4 \
  zlib1g \
  libarrow1700 \
  libarrow-flight1700 \
  libparquet1700
```

### Amazon Linux 2023 / Fedora / RHEL 9+

```bash
sudo dnf install -y \
  llvm19-libs \
  numactl-libs \
  lz4-libs \
  liburing \
  openssl-libs \
  libcurl \
  zlib
```

> **Note:** Arrow/Parquet/Flight RPMs may not be available in default repos.
> If `libarrow` is missing, install from the [Apache Arrow repo](https://arrow.apache.org/install/) or build from source.

### Verify Prerequisites

After installing, verify all shared libraries are available:

```bash
ldd ./zepto_http_server | grep "not found"
```

If the output is empty, all dependencies are satisfied.

---

## Download & Install

### 1. Download

```bash
# Latest release — amd64
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-amd64-0.0.1.tar.gz

# Latest release — arm64 (Graviton)
curl -LO https://github.com/ZeptoDB/ZeptoDB/releases/latest/download/zeptodb-linux-arm64-0.0.1.tar.gz
```

### 2. Extract

```bash
tar xzf zeptodb-linux-amd64-0.0.1.tar.gz
cd zeptodb-linux-amd64-0.0.1/
```

### 3. Included Binaries

| Binary | Description |
|--------|-------------|
| `zepto_http_server` | Main database server (HTTP API on port 8123) |
| `zepto_data_node` | Data node for distributed clusters |
| `zepto_flight_server` | Arrow Flight gRPC server |
| `zepto-cli` | Interactive SQL REPL |

### 4. Run

```bash
# Start the server
./zepto_http_server --port 8123

# In another terminal — verify
curl http://localhost:8123/ping
# → OK

# Interactive SQL
./zepto-cli --host localhost --port 8123
```

---

## Optional: Install to System Path

```bash
sudo install -m 755 zepto_http_server zepto_data_node \
  zepto_flight_server zepto-cli /usr/local/bin/
```

---

## Optional: systemd Service

Create `/etc/systemd/system/zeptodb.service`:

```ini
[Unit]
Description=ZeptoDB Time-Series Database
After=network.target

[Service]
Type=simple
User=zeptodb
ExecStart=/usr/local/bin/zepto_http_server --port 8123
Restart=on-failure
RestartSec=5
LimitNOFILE=65536
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
```

```bash
sudo useradd -r -s /sbin/nologin zeptodb
sudo systemctl daemon-reload
sudo systemctl enable --now zeptodb
sudo systemctl status zeptodb
```

---

## Troubleshooting

### Missing shared libraries

```
./zepto_http_server: error while loading shared libraries: libLLVM-19.so: cannot open shared object file
```

Install the LLVM 19 runtime package for your distribution (see [Prerequisites](#prerequisites)).

### Permission denied

```bash
chmod +x zepto_http_server zepto_data_node zepto_flight_server zepto-cli
```

### Port already in use

```bash
# Check what's using port 8123
ss -tlnp | grep 8123

# Use a different port
./zepto_http_server --port 8124
```

---

## Next Steps

- [Quick Start](https://docs.zeptodb.com/getting-started/QUICK_START/) — insert data and run queries
- [SQL Reference](../api/SQL_REFERENCE.md) — full SQL syntax
- [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md) — tuning, TLS, clustering
- [Bare-metal Tuning](../deployment/BARE_METAL_TUNING.md) — CPU pinning, NUMA, hugepages

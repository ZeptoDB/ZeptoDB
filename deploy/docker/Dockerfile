# ZeptoDB Cloud-Native Dockerfile
# Single image for all roles: master, data-node, flight, cli
#
# Enabled:  SIMD (Highway), JIT (LLVM), TLS/JWT (OpenSSL), S3 (AWS SDK), LZ4, io_uring, Web UI
# Disabled: UCX/RDMA (bare-metal only), Python binding (use pip install zeptodb)

# ============================================================================
# Stage 1: Web UI (Next.js static export)
# ============================================================================
FROM node:22-slim AS webbuilder

RUN corepack enable pnpm
WORKDIR /web
COPY web/package.json web/pnpm-lock.yaml web/pnpm-workspace.yaml ./
RUN pnpm install --frozen-lockfile
COPY web/ .
RUN pnpm build

# ============================================================================
# Stage 2: C++ Builder
# ============================================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    wget gnupg ca-certificates && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
      > /etc/apt/trusted.gpg.d/llvm.asc && \
    echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main" \
      > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && apt-get install -y --no-install-recommends \
    clang-19 llvm-19-dev \
    cmake ninja-build git pkg-config \
    libnuma-dev liblz4-dev liburing-dev libssl-dev libcurl4-openssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build Google Highway (SIMD)
RUN git clone --depth 1 --branch 1.2.0 https://github.com/google/highway.git /tmp/highway && \
    cmake -S /tmp/highway -B /tmp/highway/build -G Ninja \
      -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
      -DCMAKE_BUILD_TYPE=Release -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF && \
    ninja -C /tmp/highway/build -j$(nproc) && \
    ninja -C /tmp/highway/build install && \
    rm -rf /tmp/highway

# Build AWS SDK C++ (S3 only, static)
RUN git clone --depth 1 --branch 1.11.510 --recurse-submodules \
      https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk && \
    cmake -S /tmp/aws-sdk -B /tmp/aws-sdk/build -G Ninja \
      -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_ONLY="s3" \
      -DBUILD_SHARED_LIBS=OFF \
      -DENABLE_TESTING=OFF \
      -DAUTORUN_UNIT_TESTS=OFF && \
    ninja -C /tmp/aws-sdk/build -j$(nproc) && \
    ninja -C /tmp/aws-sdk/build install && \
    rm -rf /tmp/aws-sdk

WORKDIR /build
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tools ./tools
COPY tests ./tests
COPY third_party ./third_party

RUN mkdir -p build && cd build && \
    cmake .. \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-19 \
        -DCMAKE_CXX_COMPILER=clang++-19 \
        -DCMAKE_CXX_FLAGS="-march=x86-64-v3 -mtune=generic -O3" \
        -DAPEX_CLOUD_NATIVE=ON \
        -DAPEX_USE_HUGEPAGES=OFF \
        -DAPEX_USE_RDMA=OFF \
        -DZEPTO_USE_UCX=OFF \
        -DZEPTO_USE_JIT=ON \
        -DLLVM_DIR=/usr/lib/llvm-19/lib/cmake/llvm \
        -DZEPTO_USE_S3=ON \
        -DZEPTO_BUILD_PYTHON=OFF \
        -DAPEX_USE_IO_URING=ON \
        -DBUILD_TESTS=OFF && \
    ninja -j$(nproc) zepto_http_server zepto_data_node zepto-cli && \
    strip zepto_http_server zepto_data_node zepto-cli

# Collect runtime shared libs
RUN mkdir -p /runtime-libs && \
    for bin in build/zepto_http_server build/zepto_data_node; do \
      ldd "$bin" | grep "=> /" | awk '{print $3}' | \
      xargs -I{} cp --parents {} /runtime-libs/ 2>/dev/null; \
    done && \
    mkdir -p /runtime-libs/var/log/zeptodb && chmod 777 /runtime-libs/var/log/zeptodb

# ============================================================================
# Stage 3: Runtime (distroless)
# ============================================================================
FROM gcr.io/distroless/cc-debian12:nonroot

LABEL maintainer="ZeptoDB Team <contact@zeptodb.io>"
LABEL description="ZeptoDB — Ultra-Low Latency Time-Series Database"
LABEL version="0.0.1"
LABEL org.opencontainers.image.source="https://github.com/ZeptoDB/ZeptoDB"
LABEL org.opencontainers.image.url="https://zeptodb.io"
LABEL org.opencontainers.image.vendor="ZeptoDB"

WORKDIR /opt/zeptodb

# Binaries
COPY --from=builder /build/build/zepto_http_server .
COPY --from=builder /build/build/zepto_data_node .
COPY --from=builder /build/build/zepto-cli .

# Runtime shared libs + log dir
COPY --from=builder /runtime-libs/ /

# Web UI static files
COPY --from=webbuilder /web/out ./web/

EXPOSE 8123

ENV APEX_WORKER_THREADS=8 \
    APEX_ANALYTICS_MODE=true \
    APEX_PORT=8123

ENTRYPOINT ["./zepto_http_server"]
CMD ["--port", "8123", "--no-auth", "--web-dir", "/opt/zeptodb/web"]

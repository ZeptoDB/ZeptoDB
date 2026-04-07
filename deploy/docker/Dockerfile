# ZeptoDB Cloud-Native Dockerfile
# Multi-stage build for minimal image size

# ============================================================================
# Stage 1: Builder
# ============================================================================
FROM clang:19 AS builder

LABEL maintainer="ZeptoDB Team <contact@zeptodb.io>"
LABEL description="ZeptoDB Builder Stage"

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    libhighway-dev \
    libnuma-dev \
    liblz4-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source code
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests

# Build with cloud-native optimizations
# - march=x86-64-v3: AVX2 지원 (대부분 클라우드 인스턴스)
# - O3: 최고 최적화
# - DAPEX_CLOUD_NATIVE=ON: 클라우드 특화 설정
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
        -DAPEX_USE_IO_URING=ON \
        -DBUILD_TESTS=OFF && \
    ninja -j$(nproc) && \
    strip zepto_server *.so

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM ubuntu:22.04

LABEL maintainer="ZeptoDB Team <contact@zeptodb.io>"
LABEL description="ZeptoDB Analytics Edition - Cloud Native"
LABEL version="0.0.1"
LABEL org.opencontainers.image.source="https://github.com/ZeptoDB/ZeptoDB"
LABEL org.opencontainers.image.url="https://zeptodb.io"
LABEL org.opencontainers.image.vendor="ZeptoDB"

# Install runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    libhighway1 \
    libnuma1 \
    liblz4-1 \
    liburing2 \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -r -u 1000 -m -s /bin/bash apex && \
    mkdir -p /opt/zeptodb/data /opt/zeptodb/config && \
    chown -R apex:apex /opt/zeptodb

WORKDIR /opt/zeptodb

# Copy binaries from builder
COPY --from=builder /build/build/zepto_server .
COPY --from=builder /build/build/*.so* .

# Set ownership
RUN chown -R apex:apex /opt/zeptodb

# Switch to non-root user
USER apex

# Expose HTTP API port
EXPOSE 8123

# Health check endpoint
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8123/health || exit 1

# Environment variables (can be overridden)
ENV APEX_WORKER_THREADS=8 \
    APEX_ANALYTICS_MODE=true \
    APEX_PORT=8123

# Entrypoint
ENTRYPOINT ["./zepto_server"]

# Default command (can be overridden)
CMD ["--port", "8123", "--cloud-native"]

# ZeptoDB Cloud-Native Dockerfile
# Multi-stage build: debian builder → distroless runtime (~30MB)

# ============================================================================
# Stage 1: Builder
# ============================================================================
FROM debian:bookworm-slim AS builder

# Install LLVM 19 + build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    wget gnupg ca-certificates && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
      > /etc/apt/trusted.gpg.d/llvm.asc && \
    echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main" \
      > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && apt-get install -y --no-install-recommends \
    clang-19 llvm-19-dev cmake ninja-build git \
    libhighway-dev libnuma-dev liblz4-dev liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests

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

# Collect runtime shared libs needed by zepto_server
RUN mkdir -p /runtime-libs && \
    ldd build/zepto_server | grep "=> /" | awk '{print $3}' | \
    xargs -I{} cp --parents {} /runtime-libs/

# ============================================================================
# Stage 2: Runtime (distroless — minimal, no shell, no package manager)
# ============================================================================
FROM gcr.io/distroless/cc-debian12:nonroot

LABEL maintainer="ZeptoDB Team <contact@zeptodb.io>"
LABEL description="ZeptoDB Analytics Edition - Cloud Native"
LABEL version="0.0.1"
LABEL org.opencontainers.image.source="https://github.com/ZeptoDB/ZeptoDB"
LABEL org.opencontainers.image.url="https://zeptodb.io"
LABEL org.opencontainers.image.vendor="ZeptoDB"

WORKDIR /opt/zeptodb

# Copy binary + shared libs
COPY --from=builder /build/build/zepto_server .
COPY --from=builder /build/build/*.so* .
COPY --from=builder /runtime-libs/ /

EXPOSE 8123

ENV APEX_WORKER_THREADS=8 \
    APEX_ANALYTICS_MODE=true \
    APEX_PORT=8123

ENTRYPOINT ["./zepto_server"]
CMD ["--port", "8123", "--cloud-native"]

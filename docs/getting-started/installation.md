# Installation

## System Requirements

- **OS**: Amazon Linux 2023 / Fedora / Ubuntu 22.04+
- **Compiler**: Clang 19+ (C++20)
- **Architecture**: x86_64 or aarch64 (ARM Graviton)

## Dependencies

=== "Amazon Linux 2023 / Fedora"

    ```bash
    sudo dnf install -y clang19 clang19-devel llvm19-devel \
      highway-devel numactl-devel ucx-devel ninja-build lz4-devel
    ```

=== "Ubuntu"

    ```bash
    sudo apt install -y clang-19 llvm-19-dev libhwy-dev \
      libnuma-dev libucx-dev ninja-build liblz4-dev
    ```

## Docker

```bash
docker pull zeptodb/zeptodb:latest
docker run -p 8123:8123 zeptodb/zeptodb:latest
```

## Helm (Kubernetes)

```bash
helm install zeptodb ./deploy/helm/zeptodb
```

For production deployment details, see [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md).

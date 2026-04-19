# Contributing to ZeptoDB

Thank you for your interest in contributing to ZeptoDB! This guide will help you get started.

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/<your-username>/zeptodb.git`
3. Create a branch: `git checkout -b feature/your-feature`
4. Build the project (see below)
5. Make your changes
6. Run tests
7. Submit a Pull Request

## Build

```bash
# Dependencies (Amazon Linux 2023 / Fedora)
sudo dnf install -y clang19 clang19-devel llvm19-devel \
  highway-devel numactl-devel ucx-devel ninja-build lz4-devel

mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19
ninja -j$(nproc)
```

## Running Tests

```bash
cd build
ninja zepto_tests
./tests/zepto_tests                              # all tests (single process, serial)
./tests/zepto_tests --gtest_filter="*YourTest*"  # specific test

# Parallel — one process per test, scales with cores:
ctest -j$(nproc) --output-on-failure

# Note: a single `./tests/zepto_tests` invocation runs GTest cases
# sequentially in-process by design. Use ctest for parallel execution.

# Python tests
python3 -m pytest ../tests/test_python.py -v
```

## Running the full test matrix

For pre-commit or pre-release verification, use the single orchestrator
`tools/run-full-matrix.sh` instead of invoking every runner by hand.
It composes the build, ctest, integration shell scripts, pytest suites,
local benches, aarch64 (Graviton via EKS), and the EKS K8s benchmark
pipeline — fail-fast by default, with one shared EKS wake/sleep cycle
when cloud stages are selected. See `docs/devlog/092_full_matrix_test_script.md`
(and `docs/devlog/093_parallel_arm64_unit_stage.md` for the parallel arm64
stage) for the full design.

`--local` / `--eks` / `--all` all include a parallel arm64 unit stage
(`aarch64_unit_ssh`) by default. It rsyncs the tree to a persistent
Graviton EC2 instance, remote-builds `zepto_tests` / `test_feeds` /
`test_migration`, and runs the same ctest exclusion regex as the x86
stage — all in parallel with stage 2, at \$0 cost. If the host is
unreachable (VPN down, instance stopped) the stage prints a `WARN` and
skips with rc=0 so local development never blocks. Override the target
with `GRAVITON_HOST` / `GRAVITON_KEY` env vars, or drop the stage
entirely with `--no-arm64`.

```bash
# Local only (stages 1,2,8,3,4): build + ctest + parallel arm64 unit + integration + pytest
./tools/run-full-matrix.sh --local

# Skip the persistent-Graviton arm64 stage (VPN down, instance offline, etc.)
./tools/run-full-matrix.sh --local --no-arm64

# Local + cloud (stages 1,2,8,3,4,7): adds full EKS K8s/bench pipeline
# (Stage 6 — EKS-buildx aarch64 image — is deprecated in favor of stage 8;
#  request it explicitly with --stages=6 if you need the container path.)
./tools/run-full-matrix.sh --eks --repo=<account>.dkr.ecr.<region>.amazonaws.com/zeptodb

# Everything including local benches (stages 1,2,8,3,4,5,7)
./tools/run-full-matrix.sh --all --repo=<account>.dkr.ecr.<region>.amazonaws.com/zeptodb

# Preview without executing
./tools/run-full-matrix.sh --dry-run --all
```

Stage logs land in `/tmp/zepto_full_matrix_<timestamp>/stage_<n>_<name>.log`.
Run `tools/run-full-matrix.sh --help` for the complete flag list.

## Code Style

- C++20 standard
- Use `ZEPTO_INFO(...)` / `ZEPTO_WARN(...)` / `ZEPTO_ERROR(...)` for engine logging
- Use `zeptodb::util::Logger` for HTTP server logging
- Header files: doc comments on all public classes and functions
- Follow existing naming conventions (snake_case for functions/variables, PascalCase for classes)

## Pull Request Guidelines

- Fill out the PR template completely
- Include tests for new functionality
- Update documentation when changing behavior (see Documentation section)
- Keep PRs focused — one feature or fix per PR
- Ensure `ninja` builds cleanly with no new warnings

## Documentation

When you change code, update the relevant docs:

- `docs/COMPLETED.md` — when a feature is done
- `docs/BACKLOG.md` — remove completed items
- `docs/devlog/` — write a devlog entry for significant changes
- `docs/design/` — update design docs for architecture changes
- `docs/api/` — update API references for interface changes
- Header file comments — keep doc comments in sync

## Reporting Issues

- Use GitHub Issues with the provided templates
- Include reproduction steps, expected vs actual behavior
- For performance issues, include benchmark numbers and hardware specs

## Community

- [Discord](https://discord.gg/zeptodb) — questions, discussions, help
- [GitHub Discussions](https://github.com/zeptodb/zeptodb/discussions) — design proposals, RFCs

## License

By contributing, you agree that your contributions will be licensed under the [Business Source License 1.1](LICENSE).

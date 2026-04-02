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
./tests/zepto_tests                              # all tests
./tests/zepto_tests --gtest_filter="*YourTest*"  # specific test

# Python tests
python3 -m pytest ../tests/test_python.py -v
```

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

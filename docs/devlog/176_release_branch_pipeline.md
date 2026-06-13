# Devlog 176 — Dev Branch and Main Release Pipeline

Date: 2026-06-13
Status: Complete

## Context

Development now needs to flow through `dev`, while `main` acts as the release
branch. The existing release workflow already publishes artifacts from `v*`
tags, but there was no automation that created a synchronized version and tag
when code reached `main`.

## Changes

- Added `.github/workflows/version-main-release.yml` to bump the patch version
  on `main`, commit synchronized version files, and create `vX.Y.Z`; the tag
  push triggers the release workflow. The workflow uses `RELEASE_BOT_TOKEN`
  because the organization disables write permissions for the default
  `GITHUB_TOKEN`.
- Added `.github/scripts/release_version.py` so version calculation and file
  updates are testable outside GitHub Actions. If checked-in version files are
  ahead of the latest tag, that version is published exactly; otherwise the
  latest tag's patch component is bumped.
- Updated `.github/workflows/release.yml` to support manual workflow dispatch
  on a tag ref in addition to tag pushes.
- Updated Graviton and PR documentation checks to run against `dev` as well as
  `main`.
- Updated the local pre-push hook to block direct local pushes to `main` unless
  `ZEPTO_ALLOW_MAIN_PUSH=1` is set.
- Documented the branch flow, version policy, and required GitHub repository
  settings in `docs/deployment/RELEASE_PROCESS.md`.
- Added `docs/deployment/BRANCH_RELEASE_POLICY.md` as the initial v0.1 policy
  for `dev`, `main`, protected release tags, and the release bot.
- Increased the Graviton ARM64 workflow timeout from 15 to 30 minutes after
  the release-promotion PR showed the suite still making progress when the
  old timeout killed it.
- Removed the stale `homebrew-tap` gitlink from the repository index. The
  release workflow already updates `ZeptoDB/homebrew-tap` through
  `repository_dispatch`, and the in-repo gitlink had no `.gitmodules` entry,
  which caused checkout cleanup warnings on the self-hosted runner.
- Hardened the release binary build for CMake 3.31 and Clang 19 by installing
  `clang-tools-19` and passing `clang-scan-deps-19` explicitly to CMake. The
  first `v0.1.0` release attempt failed on ARM64 because CMake generated Ninja
  dependency-scanning rules with `CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS-NOTFOUND`.
- Hardened the Docker release image by changing the Web UI pnpm workspace from
  ignored native build scripts to an explicit `onlyBuiltDependencies` allowlist
  for `sharp` and `unrs-resolver`, pinning the Web UI package manager to
  `pnpm@10.33.0`, and by mirroring the Clang scan-deps fix in the Docker
  builder stages.

## Verification

- `python3 .github/scripts/release_version.py next` -> `0.1.0` with the
  current repository state (`v0.0.3` latest tag, checked-in `0.1.0` CMake/web
  versions).
- `python3 .github/scripts/release_version.py bump 0.1.0 --dry-run`.
- Temporary git-repo smoke: tag `v0.0.3`, calculate `0.1.0`, run a real bump,
  and verify CMake, Python, and web package versions all become `0.1.0`.
- `python3 -m py_compile .github/scripts/release_version.py`.
- YAML parse smoke for `version-main-release.yml`, `release.yml`,
  `graviton-test.yml`, and `pr-docs-check.yml`.
- `bash -n .githooks/pre-push .github/scripts/check_docs_updated.sh
  .github/scripts/check_english_first.sh`.
- Simulated `refs/heads/main` pre-push input exits 1 and prints the direct
  main-push block message.
- `git diff --check`.
- `git diff --name-only | bash .github/scripts/check_docs_updated.sh`.
- `bash .github/scripts/check_english_first.sh`.
- Release-promotion PR check diagnosis: `Graviton ARM64 Build & Test` reached
  `RebalanceTest.PartialMoveMultipleSymbols` with passing tests before the
  15-minute job timeout cancelled the run; no unit-test assertion failed.
- Release-promotion PR rerun: `Graviton ARM64 Build & Test` completed
  successfully in 17m32s after the timeout increase.
- Failed `v0.1.0` release diagnosis: ARM64 binary build failed at
  `Build ZeptoDB` with missing `clang-scan-deps`, and the AMD64 matrix leg was
  cancelled after the matrix failure.
- Failed `v0.1.1` release diagnosis: Linux AMD64 and ARM64 binary builds and
  PyPI publishing completed, then the Docker image failed during
  `pnpm install --frozen-lockfile` because pnpm 10 rejected the ignored native
  build scripts for `sharp` and `unrs-resolver`; GitHub Release and Homebrew
  dispatch were skipped behind the failed Docker job.
- `pnpm install --frozen-lockfile` in `web/` with pnpm 10.33.0.
- `pnpm build` in `web/`.
- `docker build --target webbuilder -f deploy/docker/Dockerfile .` completed
  with `sharp` and `unrs-resolver` native build scripts allowed.
- `docker build --target builder -f deploy/docker/Dockerfile .` completed the
  LLVM/Arrow/Highway/AWS SDK/ZeptoDB builder path with the Docker scan-deps
  hardening in place.

## Follow-ups

- Add a `RELEASE_BOT_TOKEN` repository secret from a release bot account with
  permission to bypass the active `main` and `v*` rulesets, or change the
  organization policy to allow workflow write tokens and narrow the ruleset
  bypass accordingly.

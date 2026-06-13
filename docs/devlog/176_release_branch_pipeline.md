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
  on `main`, commit synchronized version files, create `vX.Y.Z`, and dispatch
  the release workflow.
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

## Follow-ups

- Configure GitHub branch protection so `main` is promoted intentionally and so
  the release workflow is allowed to write the generated version commit.

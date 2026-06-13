# 178: Release Pipeline Parallel Docker Cache

Date: 2026-06-13
Status: Complete

## Context

The successful v0.1.2 release established a working `dev` to `main` promotion
pipeline, but the release workflow still serialized Docker image publication
behind the amd64 and arm64 binary matrix. In that run the slowest binary leg
took 24m31s and Docker took another 19m22s, making the critical path roughly
the sum of both stages.

The goal for this pass is to overlap Docker build work with binary builds
without allowing a failed binary release to publish Docker Hub tags.

## Changes

- Split the release Docker path into `docker-cache` and `docker` jobs.
- `docker-cache` now builds the release Dockerfile without pushing and stores
  BuildKit layers in the shared GitHub Actions cache scope while binary builds
  run in parallel.
- `docker` still waits for both the binary matrix and Docker cache warm-up
  before logging in to Docker Hub and pushing `zeptodb/zeptodb:X.Y.Z` and
  `zeptodb/zeptodb:latest`.
- Documented the split in the release process.
- Refreshed `BACKLOG.md` so the header, recent completions, next devlog hint,
  and open-item count reflect devlogs 173-178.

## Verification

- YAML parse smoke for `.github/workflows/release.yml`.
- `git diff --check`.
- `({ git diff --name-only; git ls-files --others --exclude-standard; } |
  bash .github/scripts/check_docs_updated.sh)`.
- `bash .github/scripts/check_english_first.sh`.
- `actionlint` was not installed in the local environment, so strict GitHub
  Actions linting was not run.

## Follow-ups

- Measure the next `main` promotion release to confirm the new critical path.

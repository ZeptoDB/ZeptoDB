# Devlog 177 — CI Pipeline Speedups

Date: 2026-06-13
Status: Complete

## Context

The first successful v0.1.2 release showed the release pipeline working
end-to-end, but also made the slowest stages visible: Linux amd64 binary build
completed in 24m31s, Linux arm64 binary build completed in 14m7s, and Docker
image build/push completed in 19m22s. The goal for this pass is to reduce
avoidable work without weakening release validation.

## Changes

- Added a repository `.dockerignore` so Docker release builds no longer upload
  Git metadata, local build directories, web build output, docs, benchmark
  artifacts, or Python packaging output as build context.
- Added per-architecture ccache restore/save support to the release binary
  matrix and wired CMake through `CMAKE_C_COMPILER_LAUNCHER` and
  `CMAKE_CXX_COMPILER_LAUNCHER`.
- Narrowed `Graviton ARM64 Build & Test` triggers for docs, web, deployment,
  workflow-only, and `.dockerignore` changes, and skipped release-bot
  `chore(release): vX.Y.Z` version bump commits. Those generated release
  commits are still validated by the tag-triggered amd64/arm64 release builds.
- Added Graviton workflow concurrency so stale runs for the same branch or pull
  request are cancelled when a newer run starts.

## Verification

- YAML parse smoke for `.github/workflows/release.yml` and
  `.github/workflows/graviton-test.yml`.
- `git diff --check`.
- `git diff --name-only | bash .github/scripts/check_docs_updated.sh`.
- `bash .github/scripts/check_english_first.sh`.
- `docker build --target webbuilder -f deploy/docker/Dockerfile .` completed
  with Docker build context reduced to 1.03 MB.
- `docker build --target builder -f deploy/docker/Dockerfile .` completed
  with Docker build context reduced to 5.38 MB and all required C++ source,
  test, and third-party COPY layers available.

## Follow-ups

- The Docker release job was split into parallel cache warming and gated final
  publish in devlog 178.

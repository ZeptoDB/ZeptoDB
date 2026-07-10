# 214: Release Docker Multi-Arch and Perf Smoke Closure

Date: 2026-07-10
Status: Complete

## Context

The v0.1.7 production-readiness pass left two release polish items: three feed
connector performance harnesses were still disabled by default, and Docker Hub
published only an amd64 image while binary tarballs covered amd64 and arm64.

## Changes

- Converted Kafka, MQTT, and OPC-UA hot-path perf harnesses into CI-safe smoke
  tests that run by default below TickPlant queue capacity.
- Updated the release workflow to build native amd64 and arm64 Docker images,
  publish per-architecture tags, and compose public multi-arch manifests.
- Added a workflow-dispatch path for republishing Docker images for an existing
  release version without creating a new GitHub Release, PyPI package, or tag.
- Isolated release-version Graviton workflow concurrency so skipped
  `chore(release): ...` commits do not cancel the real main-merge Graviton run.
- Documented Docker platform support in the deployment guide.

## Verification

- `git diff --check` passes.
- `.github/workflows/release.yml` and `.github/workflows/graviton-test.yml`
  parse with PyYAML.
- `ninja -C build -j$(nproc) zepto_tests` passes.
- Focused perf smoke tests pass locally: Kafka, MQTT, and OPC-UA, 3/3.
- Broad local CTest passes: 1727/1727 with `-E "Benchmark\.|K8s"`; the
  only skipped test is the S3 opt-in smoke.
- Live S3 opt-in smoke passes separately: `S3Sink.*`, 2/2, using a temporary
  test bucket with cleanup verified.
- GitHub Actions verification is required after this change is pushed.

## Follow-ups

- Use the manual release workflow with `version=0.1.7` to republish the current
  Docker tags as multi-arch manifests after this workflow reaches `main`.

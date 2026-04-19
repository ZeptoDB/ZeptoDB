# Devlog 083: Graviton ARM64 CI Fix

## Symptom

`Graviton ARM64 Build & Test` failed 6+ consecutive runs on `main`
(ref: run 24627411086). Two independent failure modes in the logs.

## Root Causes

1. **Stale `/tmp/highway` on persistent self-hosted runner.**
   `Build Highway` runs `git clone ... /tmp/highway`, which fails when the
   directory already exists. The `Free disk space` step that cleans it up
   runs *after* `Build Highway`, so a single prior failure leaves the runner
   permanently broken for subsequent runs.

2. **Broken disk-parse regex for NVMe devices in `Expand filesystem`.**
   `DISK=$(echo "$ROOT_DEV" | sed 's/p[0-9]*$//' | sed 's/[0-9]*$//')` —
   for `/dev/nvme0n1p1`, the first sed yields `/dev/nvme0n1` (correct),
   then the second sed strips the trailing `1` → `/dev/nvme0n`, which does
   not exist. `growpart` fails.

## Fixes Applied

Only `.github/workflows/graviton-test.yml` was modified.

1. Prepended `rm -rf /tmp/highway` as the first line of the `Build Highway`
   step, making it idempotent on a persistent runner.

2. Replaced the chained-sed disk parser with a bash-regex branch:
   match `^(.*)p([0-9]+)$` first (NVMe: `/dev/nvme0n1p1` → disk
   `/dev/nvme0n1`, part `1`); otherwise fall back to stripping trailing
   digits (`/dev/sda1` → `/dev/sda`, `/dev/xvda1` → `/dev/xvda`).

## Scope

CI-only fix. No engine / library / test code changed. Workflow YAML
validated with `python3 -c "import yaml; yaml.safe_load(...)"` → OK.

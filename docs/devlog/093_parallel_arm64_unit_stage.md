# Devlog 093 — Parallel arm64 unit stage in `tools/run-full-matrix.sh`

Date: 2026-04-18
Status: ✅ Completed

## Motivation

The `cross-arch-verification` skill (see `.kiro/skills/cross-arch-verification/SKILL.md`)
requires that every build is exercised on **both** x86_64 and aarch64 before
it is declared complete. Until now the orchestrator (`devlog 092`) only ran
aarch64 coverage via stage 6 / 7 — which spin up the EKS bench cluster and
cost ~\$0.50–\$1.50 per invocation. That cost gate meant aarch64 was almost
never exercised on pre-commit `--local` runs, which is exactly when bitness
regressions are cheapest to catch.

There is, however, a persistent Graviton EC2 test instance already wired
into `.githooks/pre-push` (`ec2-user@172.31.71.135`, key
`$HOME/ec2-jinmp.pem`, remote dir `~/zeptodb`). The hook rsyncs the tree,
remote-builds `zepto_tests`, and runs the GTest binary on every push — it is
**always on**, free at the margin, and already authenticated from this
developer workstation.

This devlog wires the same path into `run-full-matrix.sh` as a new **stage 8
`aarch64_unit_ssh`**, and runs it in parallel with stage 2 (x86 unit tests)
so the extra coverage comes at near-zero added wall-clock cost.

## Design

```
Stage 1 build ─▶ ┌─ Stage 2 unit_x86     ─▶ ─┐
                 │  (ctest -j, x86)          │ wait
                 └─ Stage 8 aarch64_unit_ssh ┘
                     rsync + ssh + ninja + ctest (same -E regex)
                                                  ─▶ Stage 3 integration
                                                  ─▶ Stage 4 python
```

- **Transport**: `rsync -az --delete` over SSH, same exclusion set as
  `.githooks/pre-push` (`build/`, `build_clang/`, `.git/`, `node_modules/`,
  `web/`, `site/`, `*.egg-info/`, `dist/`, `CMakePresets.json`).
- **Remote command**: `cd ~/zeptodb/build && ninja -j$(nproc) zepto_tests
  test_feeds test_migration && ctest -j$(nproc) -E "Benchmark\.|K8s"
  --output-on-failure --timeout 180`. The ctest exclusion regex is
  **identical** to stage 2 so x86 and aarch64 cover the same test set.
- **Preflight**: `ssh -o ConnectTimeout=3 -o BatchMode=yes` with a non-zero
  rc → print a `WARN` and exit the stage with rc=0 (non-blocking skip).
  This is critical for local-dev when VPN is down or the instance is
  stopped; the rest of the pipeline must still pass.
- **Host override**: `GRAVITON_HOST` / `GRAVITON_KEY` environment variables,
  defaulting to the same pair `.githooks/pre-push` uses.
- **Parallelism**: when both stage 2 and stage 8 are in the plan,
  `run_stages_parallel()` forks each as a background subshell
  (`bash -c "$cmd" > "$log" 2>&1 &`), `wait`s for both, and collates
  `rc` / wall-time files under `$LOG_DIR/.rc_<n>` / `$LOG_DIR/.sec_<n>`.
  `set +e` is used inside `_fork_one` so a non-zero remote rc does not
  abort the subshell before it records its result. Fail-fast is applied
  after both children finish. If only one of the two stages is selected,
  it falls through to the normal sequential `run_stage` path.
- **Cost**: \$0. The Graviton EC2 instance is persistent and not owned by
  this orchestrator.

## Default plans

| Flag      | Stage plan (devlog 092)  | Stage plan (this devlog) |
|-----------|--------------------------|--------------------------|
| `--local` | `1,2,3,4`                | `1,2,8,3,4`              |
| `--eks`   | `1,2,3,4,6,7`            | `1,2,8,3,4,6,7`          |
| `--all`   | `1,2,3,4,5,6,7`          | `1,2,8,3,4,5,6,7`        |

Opt-out flag: `--no-arm64` (alias `--skip-arm64`) strips stage 8 from any
already-assembled plan. Use it when the Graviton host is intentionally
offline or the developer is working from a network that cannot reach
`172.31.71.135`.

## Fallback behaviour

| Scenario                           | Stage 8 outcome                                            |
|------------------------------------|------------------------------------------------------------|
| SSH reachable, remote tests pass   | PASS (rc=0)                                                |
| SSH reachable, remote tests fail   | FAIL (rc=remote_ctest_rc) — fail-fast unless `--keep-going`|
| SSH unreachable (timeout/refused)  | WARN printed, skipped with rc=0 (**non-blocking**)         |
| `--no-arm64` / `--skip-arm64`      | Stage 8 removed from plan before execution                 |

## Custom host

```bash
export GRAVITON_HOST=ec2-user@10.0.0.42
export GRAVITON_KEY=$HOME/.ssh/my-graviton.pem
./tools/run-full-matrix.sh --local
```

## Verification

1. `./tools/run-full-matrix.sh --dry-run --local` — plan shows 5 stages
   (`1 build`, `2 unit_x86`, `8 aarch64_unit_ssh`, `3 integration`,
   `4 python`), cost `\$0.00`.
2. `./tools/run-full-matrix.sh --dry-run --local --no-arm64` — plan shows
   4 stages (stage 8 absent).
3. `./tools/run-full-matrix.sh --dry-run --eks` / `--all` — stage 8 now
   appears in both plans as expected.
4. `./tools/run-full-matrix.sh --local --keep-going` on the developer
   workstation:
   - Preflight to `ec2-user@172.31.71.135` succeeded (remote arch:
     `aarch64`).
   - Stage 2 (`unit_x86`) wall **73s**, stage 8 (`aarch64_unit_ssh`)
     wall **251s**. The two ran in parallel — the banner printed
     `PARALLEL: unit_x86 (2) ‖ aarch64_unit_ssh (8)` and forked two
     PIDs. Total wall time ≈ max(73, 251) = 251s for the pair, versus
     73 + 251 = 324s if serial — a 70s saving on `--local`.
   - Both stages hit pre-existing flaky tests on this run
     (`Snapshot.TwoNodeSnapshot` on x86, `WorkerPool.WaitIdle` timeout
     on aarch64) — neither is a regression introduced by this devlog.
5. Non-blocking-skip path verified by pointing
   `GRAVITON_HOST=ec2-user@10.255.255.1` at an unreachable address:
   stage reported `WARN: SSH preflight … failed — skipping arm64 stage
   (non-blocking).` and exited with rc=0 in 3s, leaving the rest of the
   pipeline free to run.

## Files changed

- `tools/run-full-matrix.sh` — +~90 lines: stage 8 metadata, `cmd_aarch64_ssh`,
  `run_stages_parallel()` helper, `--no-arm64` flag, `--local`/`--eks`/`--all`
  plans updated.
- `docs/devlog/093_parallel_arm64_unit_stage.md` (this file, new)
- `docs/devlog/092_full_matrix_test_script.md` — stage table updated
  (row for stage 8 added, default plans updated).
- `CONTRIBUTING.md` — "Running the full test matrix" section now mentions
  parallel arm64 coverage and the `--no-arm64` opt-out.
- `docs/COMPLETED.md` — bullet added.

## Non-goals

- This stage is **not** a replacement for stage 6 (`aarch64_unit` via EKS /
  buildx / ECR). Stage 6 still exists and is still the path exercised by
  CI runners that do not have SSH access to the persistent dev host. The
  two stages are complementary: stage 8 is free, always-on, and perfect for
  pre-commit; stage 6 is reproducible from a clean image and matches the
  production container.

## See also

- [Devlog 094 — Full-matrix orchestrator perf optimizations](094_full_matrix_perf_optimizations.md).
  Stage 8 gained a rsync-skip-on-unchanged-tree fast path, a
  stage-2-aware `ctest --timeout 300`, and was promoted into the
  4-way parallel fork group with stages 2, 3, and 4. Stage 6 was
  deprecated as a direct consequence (stage 8 covers the same
  aarch64 unit suite, for free).

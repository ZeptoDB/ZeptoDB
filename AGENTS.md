# AGENTS.md - ZeptoDB Codex Guide

This file is the primary project guide for Codex in this repository. The
legacy Kiro setup remains in `.kiro/` as source material, but Codex should use
this file first.

## Operating Rules

- Write project documentation, code comments, commit text, and generated files in English.
- Match the user's language in chat unless the user asks for project text to be produced.
- Keep code changes minimal and aligned with the surrounding file style.
- Do not modify or delete existing tests unless the user explicitly asks.
- Do not revert unrelated working tree changes. This repository may already be dirty.
- Prefer `rg` / `rg --files` for search.
- For C++ work, use the `cpp-coding-standards` skill.

## Codex Workflow

For implementation work:

1. Read the relevant design document before editing.
2. Search existing symbols and patterns before adding new APIs.
3. Implement the smallest coherent change.
4. Add or update focused tests for new logic.
5. Update related docs in the same change.
6. Run the narrowest useful build/test command and report anything not run.

Use subagents only when the user explicitly asks for delegation, parallel
agents, or multi-agent work. The old Kiro roles map to Codex work as:

- `zeptodb-dev`: planning/orchestration
- `zepto-developer`: implementation
- `zepto-reviewer`: code review
- `zepto-qa`: build, tests, and verification

The original prompts are preserved in `.kiro/context/` and `.kiro/agents/`.

## Required Documentation Sync

Code and docs must stay consistent. If code and docs disagree, verify both and
update the repository to the correct state.

When applicable, update:

- `docs/COMPLETED.md` when a feature is complete end-to-end.
- `docs/BACKLOG.md` when a completed backlog item is removed or narrowed.
- `README.md` for project-level behavior, setup, or examples.
- `docs/design/` for architecture, policy, or data-flow changes.
- `docs/devlog/` for new features, non-trivial fixes, performance work, or structural refactors.
- `docs/api/` for user-visible API changes.
- Header comments for changed public C++ classes, methods, ownership, or thread-safety semantics.

Use the `doc-update-checklist` skill after code changes.

## Research And Experiment Governance

Research work that changes runtime behavior, APIs, query planning, routing, or
telemetry must follow `docs/research/EXPERIMENT_GOVERNANCE.md`.

- Classify each experiment as `Research-only`, `Experimental runtime path`, or
  `Promoted product feature`.
- Do not describe experimental runtime paths as completed product features.
- For experimental runtime paths, document workload scope, non-goals, limits,
  telemetry, failure behavior, persistence status, rollback/disable plan, and
  product-promotion criteria.
- Mark API surfaces as experimental until the product-promotion gates pass.
- Keep `docs/BACKLOG.md` populated with remaining promotion blockers.

## Design Document Routing

Use `design-doc-index` for detailed routing. Common paths:

| Area | Read First | Code |
| --- | --- | --- |
| Storage / memory | `docs/design/layer1_storage_memory.md` | `include/zeptodb/storage/`, `src/storage/` |
| Ingestion / WAL | `docs/design/layer2_ingestion_network.md` | `include/zeptodb/ingestion/`, `src/ingestion/` |
| Execution / SIMD / JOIN | `docs/design/layer3_execution_engine.md` | `include/zeptodb/execution/`, `src/execution/` |
| SQL / Python DSL | `docs/design/layer4_transpiler_client.md` | `include/zeptodb/sql/`, `src/sql/`, `zepto_py/` |
| Security / auth | `docs/design/layer5_security_auth.md` | `include/zeptodb/auth/`, `src/auth/` |
| Cluster / distributed | `docs/design/phase_c_distributed.md` | `include/zeptodb/cluster/`, `src/cluster/` |
| Observability / HTTP | `docs/design/logging_observability.md` | `include/zeptodb/server/`, `src/server/` |
| Web UI | `docs/design/web_ui_prd.md` | `web/` |

Use `layer-patterns` before changing a specific layer.

## Build And Test

Main C++ commands:

```bash
cd build
ninja -j$(nproc)
ninja -j$(nproc) zepto_tests
./tests/zepto_tests --gtest_filter="*Pattern*"
```

Python:

```bash
cd tests/python
python -m pytest -v
```

Web:

```bash
cd web
pnpm test
pnpm lint
```

Known issue: missing `#include <vector>` in `fix_parser.h` may be pre-existing
unless the current change touches that area.

## Testing Expectations

New logic needs tests. Cover the happy path plus applicable edge cases:

- Empty or null input.
- Boundary values and capacity limits.
- Concurrency for shared state.
- Network timeouts/refusals for RPC or HTTP paths.
- Memory limits for bounded buffers and arenas.
- Type conversion and NaN/Inf behavior where relevant.
- Time edge cases for timestamp/window logic.
- Cluster, failover, and split-brain cases for distributed changes.
- Cross-architecture consistency for SIMD or layout-sensitive code.

Use `edge-case-catalog` when writing tests, and `cross-arch-verification` before
declaring cross-architecture verification complete.

## Logging Rules

| Area | Logger |
| --- | --- |
| Storage, ingestion, execution, core | `ZEPTO_INFO(...)` and related engine logger macros |
| HTTP server | `zeptodb::util::Logger` |
| Security audit | `AuditBuffer` plus spdlog logger `zepto_audit` |

Do not log raw API keys, JWTs, passwords, or secrets.

## Review Rules

When asked for a review, prioritize bugs, regressions, data loss, security,
missing tests, and doc drift. Lead with findings and file/line references.
Use `review-checklist-by-layer` for layer-specific checks.

## Local Codex Skills

Project skills live under `.agents/skills/`. The Kiro skills have been mirrored
there for Codex:

- `cpp-coding-standards`
- `cross-arch-verification`
- `design-doc-index`
- `doc-update-checklist`
- `edge-case-catalog`
- `layer-patterns`
- `review-checklist-by-layer`

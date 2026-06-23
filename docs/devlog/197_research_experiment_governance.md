# 197: Research Experiment Governance Policy

Date: 2026-06-21
Status: Complete

## Context

Action-Outcome Experiments 011 and 012 intentionally moved beyond pure research
scripts into bounded cluster runtime paths. That created useful evidence, but
also made it easy for experiment-specific behavior to look like broad product
support.

The repository now needs an explicit policy for separating research evidence,
experimental runtime validation, and promoted product features.

## Changes

- Added `docs/research/EXPERIMENT_GOVERNANCE.md`.
  - Defines `Research-only`, `Experimental runtime path`, and
    `Promoted product feature`.
  - Adds required experiment headers and status values.
  - Defines the experimental runtime path checklist and product-promotion gates.
  - Classifies the current Action-Outcome runtime changes.
- Updated `AGENTS.md` so future Codex work applies the governance policy before
  documenting or promoting experiment-driven runtime behavior.
- Reworded Action-Outcome Experiment 012 docs and references so runtime table
  placement and bounded small-table JOIN are explicitly experimental until
  product-promotion gates are met.
- Clarified that generic SQL `INSERT` materialization and string-key JOIN fixes
  are promoted product correctness work, while cluster full-data window
  materialization, bounded small-table JOIN, and runtime table placement remain
  experimental runtime paths.

## Verification

Documentation-only change. No code build was required.

```bash
rg -n "Experimental runtime path|Product feature complete|Research-only" \
  AGENTS.md docs/research/EXPERIMENT_GOVERNANCE.md docs/devlog/197_research_experiment_governance.md
```

## Follow-ups

- Add product limits and telemetry for cluster full-data window materialization.
- Add a persisted catalog/DDL placement option before promoting operational
  table placement.
- Decide whether bounded small-table JOIN should stay automatic, become a
  feature flag, or move behind an optimizer rule with explicit cost checks.

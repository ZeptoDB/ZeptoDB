# 223: Physical AI Agent Memory EKS Replay

Date: 2026-07-14
Status: Complete
Classification: Research-only

## Context

The existing Physical AI Action-Outcome fixture showed that structured context
gates can avoid unsafe action reuse, while the Agent Memory EKS suite validated
generic memory APIs. Experiment 024 connects those tracks and measures the
retrieval and compact-prior layer on the existing `zepto-bench` cluster.

## Changes

- Added `physical_ai_agent_memory_replay.py`.
  - Converts observation-only episode fields into deterministic
    64-dimensional feature-hash embeddings.
  - Excludes action, outcome, expected-safe labels, and reflection text from
    retrieval embeddings.
  - Loads historical episodes through the Agent Memory HTTP API.
  - Compares no memory, raw retrieval, outcome prior, and context-gated prior.
  - Records insert, cold/warm search, and compact-prior latency.
- Added `test_k8s_physical_ai_memory.py` for real amd64/arm64 ECR image runs.
- Added `run_physical_ai_memory_replay.sh` with unconditional EKS namespace and
  NodePool cleanup.
- Added a fail-closed preflight that verifies the expected AWS account and
  `zepto-bench` API endpoint before any cluster mutation or cleanup trap.
- Made namespaces run-owned, serialized shared-NodePool use with an
  owner-checked lock and idle checks, and made result publication no-clobber.
- Added focused Python coverage for deterministic embeddings, label exclusion,
  boundary validation, empty fixtures, percentile behavior, and HTTP failure.
- Added Experiment 024 procedure and immutable result documents.

## Verification

```bash
PYTHONPATH=/home/ec2-user/zeptodb \
  python3 -m pytest -q tests/python/test_physical_ai_agent_memory_replay.py

python3 -m py_compile \
  docs/research/tools/physical_ai_agent_memory_replay.py \
  tests/k8s/test_k8s_physical_ai_memory.py

bash -n tests/k8s/run_physical_ai_memory_replay.sh

tests/k8s/run_physical_ai_memory_replay.sh
```

Results:

- Focused Python tests: pass, 5/5.
- Existing EKS Agent Memory foundation check: pass, amd64/arm64 2/2.
- Experiment 024 amd64 warm search p50/p95: 8.028/8.519 ms.
- Experiment 024 arm64 warm search p50/p95: 9.426/18.023 ms.
- Context-gated recovery Top-1: 1.00 on both architectures.
- Context-gated hazardous top-action rate: 0.00 on both architectures.
- Cross-architecture context-gated decisions: exact match, 5/5.
- Final bench NodePool limits: x86=0, arm64=0.
- Experiment namespaces and bench NodeClaims: deleted.

## Experimental Boundary

- Intended workload: a 20-memory, 5-query synthetic Physical AI replay.
- Non-goals: VLA model quality, real image encoding, simulator task success,
  GPU efficiency, production latency SLOs, or Agent Memory product promotion.
- Hard limits: `top_k=8`, 20 warm repeats per query, 64 embedding dimensions,
  one ZeptoDB pod per architecture.
- Telemetry: client-round-trip insert/search latency and local compact-prior
  compute latency.
- Failure behavior: HTTP, deployment, empty-result, or acceptance failures
  terminate the run; the shell trap still deletes namespaces and sleeps EKS.
- Persistence: hostPath is temporary and removed with bench nodes.
- Rollback/disable: remove the research harness; no runtime behavior or public
  API was changed.

## Follow-Ups

- Replace deterministic embeddings with a real vision-language encoder.
- Add held-out simulator episodes and a GPU-backed VLA inference arm.
- Measure task success, model tokens, GPU time, total decision latency, and
  cost per episode.

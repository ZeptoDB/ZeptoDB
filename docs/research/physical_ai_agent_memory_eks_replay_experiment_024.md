# Experiment 024: Physical AI Agent Memory EKS Replay

Date: 2026-07-14
Status: Research complete
Classification: Research-only

## Goal

Measure whether ZeptoDB Agent Memory can provide a low-latency, replayable
action-outcome prior for Physical AI and future VLA experiments on the existing
`zepto-bench` EKS environment.

## Hypothesis

A compact context-gated action prior built from retrieved robot episodes should
avoid previously unsafe actions while adding less than 100 ms p95 retrieval
latency through the initial kubectl port-forward measurement path.

## Scope

This first run deliberately isolates the memory layer:

- real ZeptoDB ECR images on amd64 and arm64 EKS nodes,
- the existing 25-episode Physical AI fixture,
- 20 historical memories and 5 held-out query episodes,
- deterministic 64-dimensional observation feature hashing,
- no external embedding provider, VLA model, simulator, or GPU.

The deterministic embedding is a vision-language proxy, not evidence about a
specific vision encoder or VLA policy.

## Variants

| Variant | Meaning |
| --- | --- |
| `no_memory` | Reuse the query episode's original failed action. |
| `raw_retrieval` | Use the action from the top Agent Memory match. |
| `outcome_prior` | Aggregate retrieved actions using historical outcomes. |
| `context_gated_prior` | Gate historical outcomes by topology, change context, temporal motif, and safety labels before aggregation. |

## Procedure

1. Acquire the shared-bench lock, verify that the persistent bench NodePools,
   Helm releases, load generator, NodeClaims, and prior experiment namespaces
   are idle, then wake the x86 and arm64 NodePools.
2. Deploy one real ZeptoDB Agent Memory pod per architecture in unique,
   run-owned namespaces.
3. Convert observation-only episode fields into deterministic 64-dimensional
   embeddings. Action, outcome, expected-safe labels, and reflection text are
   excluded from the embedding.
4. Insert the 20 non-query episodes into Agent Memory.
5. Search each of the 5 held-out queries with `top_k=8`.
6. Repeat each query 20 times for warm client-round-trip latency.
7. Compute all four decision variants from the returned memory records.
8. Compare decision quality and latency across architectures.
9. Delete only the run-owned namespaces, return both NodePools to
   `CPU_LIMIT=0`, and release the shared-bench lock.

## Acceptance Criteria

| Criterion | Required |
| --- | --- |
| amd64 and arm64 runs complete | pass |
| All five held-out queries return memories | pass |
| Context-gated recovery Top-1 hit rate | 1.00 |
| Context-gated hazardous top-action rate | 0.00 |
| Warm search p95 through port-forward | less than 100 ms |
| Context-gated decisions match across architectures | pass |
| EKS NodePools after cleanup | `CPU_LIMIT=0` |

## Failure Behavior

The runner refuses an unexpected AWS account or EKS endpoint, a busy or stale
shared-bench state, a concurrently held bench lock, and an existing result
path. Cleanup targets only the current run's namespaces and releases only a
lock whose owner matches the current run. Result publication is exclusive and
does not overwrite preserved evidence.

## Command

```bash
tests/k8s/run_physical_ai_memory_replay.sh
```

## Result

See `docs/research/results/physical_ai_agent_memory_eks_replay_024.md`.

Summary:

- amd64 warm search p50/p95: 8.028/8.519 ms.
- arm64 warm search p50/p95: 9.426/18.023 ms.
- Compact-prior compute p95: 0.419 ms on amd64 and 0.317 ms on arm64.
- `no_memory` and ungated `outcome_prior` both recorded 0.00 recovery Top-1,
  0.00 risky-repeat avoidance, and 1.00 hazardous top-action rate.
- `raw_retrieval` and `context_gated_prior` both recorded 1.00 recovery Top-1,
  1.00 risky-repeat avoidance, and 0.00 hazardous top-action rate.
- Context-gated decisions matched across amd64 and arm64.
- All acceptance criteria passed.
- Both bench NodePools were returned to `CPU_LIMIT=0`; experiment namespaces
  and bench NodeClaims were deleted.

## Interpretation

The EKS retrieval layer is fast enough to proceed to a real model experiment
for this small 20-memory fixture. Raw Top-1 retrieval was already correct, but
the ungated outcome aggregate repeated the unsafe action because several
historically successful actions came from incompatible safety contexts. The
context gate recovered the safe action for all five queries.

This result does not show that a VLA model becomes faster or more accurate.
It isolates memory retrieval and compact-prior computation. Model inference,
GPU cost, token reduction, and simulator task success remain unmeasured.

## Next Research Step

Replace the proxy embedding with a real vision-language encoder, add held-out
simulator episodes, and measure end-to-end VLA task success, model latency, GPU
time, input tokens, and total decision latency.

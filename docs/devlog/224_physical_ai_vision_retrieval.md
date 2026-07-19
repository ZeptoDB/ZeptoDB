# 224: Physical AI Vision Retrieval EKS Harness

Date: 2026-07-16
Status: Complete

## Context

Experiment 024 validated ZeptoDB Agent Memory with deterministic proxy
embeddings. Experiment 025 replaces that proxy with real LIBERO images and a
SigLIP encoder on an EKS GPU node.

## Changes

- Added a research-only LIBERO sampling, SigLIP encoding, retrieval, ZeptoDB
  measurement, and report tool.
- Added deterministic balanced splitting and retrieval-metric tests.
- Added an EKS runner that returns compact results through the Kubernetes
  termination message and owns cleanup of all temporary resources.
- Added a fail-closed preflight that verifies the expected AWS account and
  `zepto-bench` API endpoint before any cluster mutation or cleanup trap.
- Removed shared-bench shutdown from the owned-NodePool cleanup path and made
  canonical report publication atomic and no-clobber.
- Changed the runner to use an Intel `c7i.xlarge` NodePool for the current
  ZeptoDB bench image and a separate `g6e.xlarge` NodePool for CUDA.
- Added bounded dataset-server request pacing plus `Retry-After`-aware
  exponential retry after the first two-node run encountered a transient row
  request failure.
- Added the missing protobuf runtime and moved model initialization ahead of
  frame collection so dependency failures occur before the long data stage.
- Moved dataset-server row resolution ahead of AWS provisioning and mounts the
  resulting URL manifest into the GPU Job, removing the row API from the
  billable EKS execution path.

## Verification

- Focused Python tests: 10 passed.
- Python compile check: passed.
- Shell syntax check: passed.
- Initial EKS attempt: stopped before ML execution because the Intel-optimized
  ZeptoDB image exited with code 132 on the AMD-hosted `g6e.xlarge`.
- First two-node EKS attempt: ZeptoDB and the L40S Job both started correctly,
  then stopped on a dataset-server row request after five retries.
- Paced-download attempt: all 290 frames loaded, then model initialization
  exposed a missing protobuf Python dependency.
- Model-first attempt: SigLIP loaded successfully, but the EKS source address
  again failed the same dataset-server offset after ten retries. Local URL
  resolution for that offset remained healthy.
- Final EKS run: pass. The local manifest contained 190 memories and 100
  queries across 10 tasks. Image-plus-instruction ZeptoDB Recall@1/Recall@5
  was 0.90/1.00, search p95 was 1.361 ms, and SigLIP encoding averaged
  1.478 ms per image on an NVIDIA L40S.
- Cleanup verification: namespace, NodePool, NodeClaim, node, and temporary
  `c7i.xlarge` and `g6e.xlarge` EC2 instances deleted; shared bench NodePool
  CPU limits are 0.

## Follow-ups

- Run Experiment 026 with a real VLA policy model.
- Preserve exact-versus-bounded-ANN quality as an explicit comparison because
  image-only Recall@1 was 0.97 locally and 0.70 through the bounded ZeptoDB ANN
  path.

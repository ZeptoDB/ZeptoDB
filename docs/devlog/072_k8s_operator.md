# Devlog 072: K8s Operator

Date: 2026-04-16

## Summary

Implemented a minimal bash-based K8s operator for ZeptoDB. The operator watches `ZeptoDBCluster` custom resources (apiVersion `zeptodb.com/v1alpha1`) and reconciles them into Helm chart releases.

## What Was Built

1. **CRD** (`deploy/operator/crd.yaml`) — `ZeptoDBCluster` with spec (replicas, version, storage, license) and status subresource (phase, readyReplicas, edition, message). Printer columns for `kubectl get zdb`.

2. **Reconciler** (`deploy/operator/reconciler.sh`) — ~120-line bash script that polls every 10s, reads CR specs, enforces license gating for multi-node, runs `helm upgrade --install`, and updates CR status. Detects deletions and runs `helm uninstall`.

3. **RBAC** (`deploy/operator/rbac.yaml`) — ServiceAccount, ClusterRole, ClusterRoleBinding. Minimal permissions: CRD watch/patch, workload CRUD, secret read.

4. **Deployment** (`deploy/operator/deployment.yaml`) — Single-replica pod using `bitnami/kubectl:1.30`. Reconciler mounted via ConfigMap.

5. **Examples** — `basic-cluster.yaml` (3-node Enterprise with license secret) and `single-node.yaml` (1-node Community, no license).

## License Gate Design

- `replicas == 1` → Community, always allowed
- `replicas > 1` → requires `spec.license.secretName` pointing to an existing K8s Secret
- Operator mounts the secret as `ZEPTODB_LICENSE_KEY` env var via Helm `extraEnv`
- The ZeptoDB binary does the actual RS256 JWT validation at startup
- If no license secret: CR status set to `Failed` with descriptive message

## Design Decisions

- **Bash over Go**: No build dependency, trivial to modify, sufficient for CRD→Helm translation
- **Poll over watch**: `kubectl get --watch` has reconnection edge cases; simple poll loop is more robust for a shell operator
- **Helm as backend**: Reuses existing chart (StatefulSet, ConfigMap, PDB, HPA) — operator only translates CR fields to `--set` flags
- **No JWT validation in operator**: Separation of concerns — operator handles K8s orchestration, binary handles cryptographic validation

## Files Created

| File | Lines | Purpose |
|------|------:|---------|
| `deploy/operator/crd.yaml` | 72 | CRD definition |
| `deploy/operator/reconciler.sh` | 125 | Bash reconciler |
| `deploy/operator/rbac.yaml` | 40 | RBAC manifests |
| `deploy/operator/deployment.yaml` | 53 | Operator Deployment |
| `deploy/operator/examples/basic-cluster.yaml` | 16 | 3-node example |
| `deploy/operator/examples/single-node.yaml` | 9 | 1-node example |
| `docs/design/k8s_operator.md` | 77 | Design doc |

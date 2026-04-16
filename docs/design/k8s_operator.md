# K8s Operator Design

## Overview

Minimal bash-based K8s operator that watches `ZeptoDBCluster` custom resources and reconciles them into Helm chart releases. No Go/Rust dependencies — the reconciler is a single shell script.

## CRD Schema

```
apiVersion: zeptodb.com/v1alpha1
kind: ZeptoDBCluster
spec:
  replicas     int      # Node count (1 = Community, >1 = Enterprise)
  version      string   # Image tag
  storage      string   # PVC size
  license:
    secretName  string   # K8s Secret with JWT license key
    secretKey   string   # Key within secret (default: "license-key")
status:
  phase         string   # Pending | Running | Failed
  readyReplicas int
  edition       string   # community | enterprise
  message       string   # Error details on failure
```

## Reconciler Flow

```
poll loop (every 10s)
  ├─ kubectl get zeptodbclusters
  ├─ for each CR:
  │   ├─ read spec (replicas, version, storage, license)
  │   ├─ if replicas > 1:
  │   │   ├─ check license.secretName is set
  │   │   ├─ check secret exists in namespace
  │   │   └─ FAIL if either missing
  │   ├─ helm upgrade --install zdb-{name} with derived values
  │   └─ update CR status (phase, readyReplicas, edition)
  └─ detect deleted CRs → helm uninstall
```

## License Gating

| Replicas | License Required | Edition |
|:--------:|:----------------:|---------|
| 1 | No | Community |
| >1 | Yes (secret must exist) | Enterprise |

The operator does NOT validate the JWT. It ensures the license secret is mounted as `ZEPTODB_LICENSE_KEY` env var. The ZeptoDB binary performs RS256 validation at startup and refuses to join a cluster with an invalid license.

This matches the license system design in `docs/design/license_system.md`:
- `Feature::CLUSTER = 1 << 0` requires Enterprise edition
- Key loading priority: env `ZEPTODB_LICENSE_KEY` → file → direct

## RBAC

The operator ServiceAccount needs:
- `zeptodb.com` group: get/list/watch/patch ZeptoDBCluster + status subresource
- `apps`: CRUD on Deployments, StatefulSets (Helm-managed)
- Core: CRUD on Services, ConfigMaps, PVCs, Pods; get on Secrets

## Deployment

Single-replica Deployment. Two options:

1. **Production**: Build custom image via `deploy/operator/Dockerfile` (includes kubectl, helm, jq, chart, reconciler)
2. **Dev**: Use init container to fetch chart + ConfigMap for reconciler script

The Helm chart templates support `extraEnv` for injecting the license key as `ZEPTODB_LICENSE_KEY` env var into ZeptoDB pods.

## Files

```
deploy/operator/
├── crd.yaml                        # ZeptoDBCluster CRD
├── rbac.yaml                       # ServiceAccount + ClusterRole + Binding
├── deployment.yaml                 # Operator Deployment
├── reconciler.sh                   # Bash reconciler (~120 lines)
├── Dockerfile                      # Operator image (alpine + kubectl + helm + jq)
└── examples/
    ├── basic-cluster.yaml          # 3-node Enterprise
    └── single-node.yaml            # 1-node Community
```

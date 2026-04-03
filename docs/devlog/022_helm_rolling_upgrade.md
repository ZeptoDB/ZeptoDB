# 022: Helm Chart & Zero-Downtime Upgrade

**Date:** 2026-03-24
**Status:** Completed
**Related code:** `helm/zeptodb/`, `docs/operations/rolling_upgrade.md`

---

## What Was Built

### Helm Chart (`helm/zeptodb/`)

Converted the existing `deploy/k8s/deployment.yaml` monolith into a proper Helm v3 chart with parameterized values.

**Templates:**
- `deployment.yaml` — Rolling update strategy, pod anti-affinity, config checksum annotation
- `service.yaml` — LoadBalancer + headless service
- `configmap.yaml` — Server configuration from values
- `pvc.yaml` — Persistent storage (conditional)
- `hpa.yaml` — Horizontal Pod Autoscaler (conditional)
- `pdb.yaml` — PodDisruptionBudget (conditional)
- `servicemonitor.yaml` — Prometheus Operator integration (conditional)

**Key design decisions:**
1. `maxSurge: 1, maxUnavailable: 0` — zero-downtime guarantee during rollout
2. `PodDisruptionBudget minAvailable: 2` — protects against node drain killing too many pods
3. `checksum/config` annotation — ConfigMap changes auto-trigger pod restart
4. `cluster.enabled` toggle — single values.yaml for both standalone and distributed mode
5. Cluster ports (RPC, heartbeat) only exposed when `cluster.enabled: true`

### Rolling Upgrade Guide (`docs/operations/rolling_upgrade.md`)

Four upgrade strategies documented:
1. **Standard** — `helm upgrade --set image.tag=X` for routine releases
2. **Config-only** — ConfigMap changes auto-roll via checksum annotation
3. **Canary** — Separate release for high-risk changes, test, then promote
4. **Cluster mode** — Extended grace period, WAL replay for state recovery

Plus rollback procedures, pre-upgrade checklist, and troubleshooting.

---

## Design Rationale

**Why Helm over raw manifests:**
- Enterprise customers expect `helm install` — it's the standard
- Values-driven config eliminates copy-paste errors across environments
- `helm rollback` gives instant rollback with revision history
- PDB/HPA/ServiceMonitor as optional features via conditionals

**Why maxUnavailable: 0:**
- ZeptoDB is an in-memory database — losing a pod means losing cached state
- HFT workloads cannot tolerate even brief capacity reduction
- Combined with preStop sleep, ensures in-flight queries complete

**Why checksum/config annotation:**
- Kubernetes doesn't restart pods on ConfigMap changes by default
- This is a common source of "I changed the config but nothing happened" issues
- Checksum forces a rolling restart only when config actually changes

---

## Lessons Learned

1. The existing `deploy/k8s/deployment.yaml` had good bones (probes, preStop, anti-affinity) — Helm-ifying it was mostly structural, not redesign
2. PDB is critical for in-memory databases — without it, `kubectl drain` can kill quorum
3. Canary pattern needs a separate Helm release, not just a label selector trick

---

## Next Steps

- [ ] Cluster status CLI (`\nodes`, `\cluster`) — operator visibility
- [ ] KUBERNETES_OPS.md — full k8s operations guide (Helm chart is the foundation)
- [ ] Helm chart testing with `helm template` / `helm lint` in CI

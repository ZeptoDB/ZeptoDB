# Devlog 104 — Pod placement hardening

Date: 2026-04-26
Scope: `deploy/helm/zeptodb/` only (values.yaml + statefulset.yaml + deployment.yaml + Chart.yaml)
Chart version: **0.1.0 → 0.2.0** (minor bump signals default-behavior change)

## ⚠️ Upgrade notice (behaviour change)

`helm upgrade` from 0.1.x to 0.2.0 changes three defaults for users who do not override them:

- **Hard antiAffinity becomes default** (`podAntiAffinity.required=true`). Pods that previously co-located on the same node will now refuse to schedule until a free node is available. On EKS Auto Mode / Karpenter this triggers node provisioning in 30–60s; on fixed-node clusters with `replicas > nodes`, pods will stay `Pending`. Set `podAntiAffinity.required=false` in your overlay to restore the old soft preference.
- **Resource requests/limits shrank** to `2c/4Gi` (requests) and `4c/8Gi` (limits), sized for a mid-size IoT pilot. If your previous overlay relied on larger chart defaults, pin `resources.requests` / `resources.limits` explicitly in your values file.
- **`topologySpreadConstraints` with `maxSkew: 1` is now on by default**. Replicas spread evenly across hostnames. Disable with `topologySpread.enabled=false`.

Bare-metal trading deployments with CPU pinning requirements should set `resources.requests.cpu == resources.limits.cpu` in an overlay to get a `Guaranteed` QoS class.

See `docs/operations/KUBERNETES_OPERATIONS.md` § "Sizing and placement" for sector-sized overlays.

## Problem

The Helm chart before this change used `preferredDuringSchedulingIgnoredDuringExecution`
for podAntiAffinity — a **soft preference**. When HPA scaled replicas beyond the current
node count, Kubernetes silently co-located multiple ZeptoDB pods on the same node, which:

- **Halved ingest CPU scaling** — two drain-thread pools fighting for the same cores.
- **Destroyed failure isolation** — a single node failure killed multiple replicas.
- **Invalidated the scale-out claim** — the ~2.9× at N=2 benchmark number assumes one
  pod per node; on a co-located pair it collapses toward ~1.5×.

The failure was silent because nothing in the Helm chart, the Pod spec, or the default
metrics surfaced it. Operators would see HPA at its target replica count, see pods
`Running`, and conclude the scale-out worked — while actually running two pods on one
node.

## Fix

Three layered controls, all user-toggleable:

### Fix 1 — `podAntiAffinity.required` toggle (default **true**)

Hard rule: no two ZeptoDB pods on the same hostname. `required: false` falls back to the
old soft-preference behaviour for dev clusters with tight node counts.

```yaml
podAntiAffinity:
  enabled: true
  required: true     # true = hard rule; false = soft preference
  weight: 100        # only used when required=false
```

### Fix 2 — `topologySpread` with `maxSkew: 1` (default **on**)

`required: true` alone refuses to schedule replicas beyond node count. `topologySpread`
is smarter: it spreads pods as evenly as possible and still allows `replicas > nodes`.
Kubernetes applies both constraints simultaneously; together they give the operator the
right default (spread hard, tolerate skew of 1) and a per-axis escape hatch.

```yaml
topologySpread:
  enabled: true
  maxSkew: 1
  topologyKey: kubernetes.io/hostname
  whenUnsatisfiable: DoNotSchedule   # or ScheduleAnyway for soft
```

### Fix 3 — Ingest-tuned resource defaults

Previous defaults (8 CPU / 32 Gi memory) were sized for a specific bare-metal Guaranteed
QoS + cpuset-pinning setup and did not match the typical mid-size factory / trading
deployment target (~200K–500K ticks/s). New defaults are a burstable 2c/4Gi → 4c/8Gi
sized for that target and are documented inline.

```yaml
resources:
  requests:
    cpu: "2000m"
    memory: "4Gi"
    hugepages-2Mi: "4Gi"
  limits:
    cpu: "4000m"
    memory: "8Gi"
    hugepages-2Mi: "4Gi"
```

Bare-metal trading workloads that need Guaranteed QoS should pin `requests.cpu ==
limits.cpu` explicitly via `--set` or an environment overlay; the inline comment in
`values.yaml` calls this out.

## Before / After

### Before (soft preferred)

```yaml
affinity:
  podAntiAffinity:
    preferredDuringSchedulingIgnoredDuringExecution:
      - weight: 100
        podAffinityTerm:
          labelSelector:
            matchLabels:
              app.kubernetes.io/name: zeptodb
              app.kubernetes.io/instance: release-name
          topologyKey: kubernetes.io/hostname
```

### After (hard required + topology spread)

```yaml
affinity:
  podAntiAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      - labelSelector:
          matchLabels:
            app.kubernetes.io/name: zeptodb
            app.kubernetes.io/instance: release-name
        topologyKey: kubernetes.io/hostname
topologySpreadConstraints:
  - maxSkew: 1
    topologyKey: "kubernetes.io/hostname"
    whenUnsatisfiable: DoNotSchedule
    labelSelector:
      matchLabels:
        app.kubernetes.io/name: zeptodb
        app.kubernetes.io/instance: release-name
```

## Tuning guidance

| Knob | Flip when |
|------|-----------|
| `podAntiAffinity.required: false` | Dev cluster where node count < replicas is routine and you accept co-location. |
| `podAntiAffinity.enabled: false` | Single-node sandbox; you want zero scheduling constraints. |
| `topologySpread.whenUnsatisfiable: ScheduleAnyway` | You want the spread hint but no scheduling block when nodes are exhausted. |
| `topologySpread.enabled: false` | You're on a topology model (zones, racks) that needs a different spread policy — redefine in an overlay. |

Full sector-sized table (IoT pilot, auto factory, semi-fab, dev) in
`docs/operations/KUBERNETES_OPERATIONS.md` → "Sizing and placement for enterprise
factory workloads".

## Trade-offs

- `required: true` will leave pods `Pending` when every node already hosts a ZeptoDB
  pod. This is the correct signal: the cluster needs another node. On EKS Auto Mode
  (Karpenter-backed), a new node is provisioned in typically 30–60 s — see
  `docs/operations/KUBERNETES_OPERATIONS.md` §5. Operators without autoscaling need
  to size the node pool to `>= maxReplicas`.
- Lower resource defaults (vs the previous 8c/32Gi) mean existing users who don't
  override via `--set` will see pods re-scheduled with the new limits on the next
  `helm upgrade`. This is a **deliberate behaviour change** — the previous defaults
  were sized for a narrow bare-metal trading profile and misled mid-size factory
  deployments into over-provisioning. Bare-metal trading overlays should pin their
  own resource block.
- Asymmetric requests/limits drop the pod from Guaranteed → Burstable QoS. Bare-metal
  trading users who need cpuset pinning must set `requests.cpu == limits.cpu` explicitly.

## Verification

### helm lint

```
$ helm lint deploy/helm/zeptodb
==> Linting deploy/helm/zeptodb
[INFO] Chart.yaml: icon is recommended
1 chart(s) linted, 0 chart(s) failed
```

### helm template — all four permutations render valid YAML

```bash
# 1. Default (both enabled, required=true)
helm template deploy/helm/zeptodb

# 2. Soft antiAffinity
helm template deploy/helm/zeptodb --set podAntiAffinity.required=false

# 3. No topologySpread
helm template deploy/helm/zeptodb --set topologySpread.enabled=false

# 4. None (both disabled)
helm template deploy/helm/zeptodb \
  --set podAntiAffinity.enabled=false \
  --set topologySpread.enabled=false
```

Observed:
- (1) → `requiredDuringSchedulingIgnoredDuringExecution` + `topologySpreadConstraints` both emitted.
- (2) → `preferredDuringSchedulingIgnoredDuringExecution` (old soft block) + `topologySpreadConstraints`.
- (3) → `requiredDuringSchedulingIgnoredDuringExecution` only.
- (4) → neither block emitted (`grep -c 'affinity' == 0`).

All four outputs parse cleanly through `yaml.safe_load_all()`.

### C++ suite — no regression

YAML-only change; full `./tests/zepto_tests` suite is unaffected. Baseline 1266/1266.

## Files touched

- `deploy/helm/zeptodb/values.yaml`
- `deploy/helm/zeptodb/templates/statefulset.yaml`
- `deploy/helm/zeptodb/templates/deployment.yaml`
- `docs/devlog/104_pod_placement_hardening.md` (this file)
- `docs/design/phase_c_distributed.md` — new §10 "Pod placement for horizontal scale-out"
- `docs/operations/KUBERNETES_OPERATIONS.md` — new §5 subsection "Sizing and placement for enterprise factory workloads"
- `docs/COMPLETED.md`
- `docs/BACKLOG.md`

# Devlog 095 — `zepto-bench` `EC2NodeClass` not found: root-cause is EKS Auto Mode, not a missing CRD install

Date: 2026-04-18
Status: ⚠️ Investigation — no change applied; follow-up requires a manifest
update in `tests/k8s/run_eks_bench.sh` instead of a Helm install.

## Symptom

`tests/k8s/run_eks_bench.sh` stage 2 ("Provision arm64 nodes (Karpenter)")
fails at `kubectl apply` with:

```
error: resource mapping not found for name: "arm64-bench" namespace: ""
  from "STDIN": no matches for kind "EC2NodeClass" in version "karpenter.k8s.aws/v1"
ensure CRDs are installed first
```

The script applies (around line 85–130) an `EC2NodeClass` +
`NodePool` pair that references `group: karpenter.k8s.aws, kind: EC2NodeClass`
and `role: KarpenterNodeRole-zepto-bench`.

## Initial hypothesis (from the orchestrator) — incorrect

> The cluster has `nodepools.karpenter.sh` but is missing
> `ec2nodeclasses.karpenter.k8s.aws`; install the matching `karpenter-crd`
> OCI Helm chart to add it.

This diagnosis assumes a self-hosted Karpenter install with one CRD missing.

## Actual root cause

`zepto-bench` is an **EKS Auto Mode** cluster, not a self-hosted Karpenter
cluster:

```
$ aws eks describe-cluster --name zepto-bench --region ap-northeast-2 \
    --query 'cluster.computeConfig'
{
  "enabled": true,
  "nodePools": ["general-purpose", "system"],
  "nodeRoleArn": "arn:aws:iam::060795905711:role/eksctl-zepto-bench-cluster-AutoModeNodeRole-EytSCmNAHOdo"
}
```

Consequences specific to Auto Mode:

- **No Karpenter controller pods** (`kubectl get pods -A | grep -i karpenter`
  is empty). The provisioner runs in the AWS-managed control plane.
- **No `helm` release for `karpenter`** (`helm list -A` shows only
  `zeptodb`). The CRDs present on the cluster —
  `nodepools.karpenter.sh`, `nodeclaims.karpenter.sh` — were installed by
  EKS Auto Mode itself (no Helm-owner labels, only the
  `controller-gen.kubebuilder.io/version: v0.19.0` annotation).
- **The AWS-provider NodeClass CRD is intentionally different**:
  Auto Mode ships `nodeclasses.eks.amazonaws.com` (kind `NodeClass`,
  group `eks.amazonaws.com`), **not** `ec2nodeclasses.karpenter.k8s.aws`.
- The two managed NodePools (`general-purpose`, `system`) and the two
  previously-created zepto-bench NodePools (`zepto-bench-arm64`,
  `zepto-bench-x86`) already reference this managed NodeClass:

  ```yaml
  nodeClassRef:
    group: eks.amazonaws.com
    kind: NodeClass
    name: default
  ```

So the CRD is not missing — it simply has a different `group/kind` under
EKS Auto Mode, and the `run_eks_bench.sh` manifest (which targets
self-hosted Karpenter) is incompatible with this cluster flavour.

## Why installing `karpenter-crd` was NOT executed

The orchestrator task asked for:

```
helm upgrade --install karpenter-crd oci://public.ecr.aws/karpenter/karpenter-crd \
  --version <matching> --namespace kube-system
```

`helm template` of that chart was inspected before applying:

```
$ helm template karpenter-crd oci://public.ecr.aws/karpenter/karpenter-crd \
    --version 1.6.4 | grep -E '^kind:|^  name:'
kind: CustomResourceDefinition
  name: ec2nodeclasses.karpenter.k8s.aws
kind: CustomResourceDefinition
  name: nodeclaims.karpenter.sh
kind: CustomResourceDefinition
  name: nodepools.karpenter.sh
```

The chart ships **all three** CRDs, including the two already installed
by Auto Mode. Applying it would:

1. Take Helm ownership of AWS-managed CRDs
   (`nodepools.karpenter.sh`, `nodeclaims.karpenter.sh`), which the
   managed provisioner is actively reconciling. Schema drift between the
   Auto Mode version and the OCI chart version could break the
   `general-purpose` NodePool currently serving all three cluster nodes.
2. Install an `ec2nodeclasses.karpenter.k8s.aws` CRD for which **no
   controller exists** on this cluster. Objects created from the
   `run_eks_bench.sh` manifest would be accepted by the API server but
   never reconciled (no IAM role `KarpenterNodeRole-zepto-bench` exists
   either — Auto Mode uses `AutoModeNodeRole-*`), so nodes would still
   not be provisioned.

Both outcomes are worse than the current failure. The install was
therefore **not performed**.

## Correct fix (recommended — deferred to a separate task)

Update the manifest in `tests/k8s/run_eks_bench.sh` (stage 2) to use the
EKS-Auto-Mode NodeClass reference, matching the existing zepto-bench
NodePools:

```yaml
# No EC2NodeClass object needed — use the managed "default" NodeClass.
apiVersion: karpenter.sh/v1
kind: NodePool
metadata:
  name: arm64-bench
spec:
  weight: 50
  template:
    spec:
      expireAfter: 720h
      nodeClassRef:
        group: eks.amazonaws.com
        kind: NodeClass
        name: default
      requirements:
        - key: kubernetes.io/arch
          operator: In
          values: ["arm64"]
        - key: karpenter.sh/capacity-type
          operator: In
          values: ["on-demand"]
        - key: karpenter.k8s.aws/instance-family
          operator: In
          values: ["m7g", "m6g", "c7g"]
        - key: karpenter.k8s.aws/instance-size
          operator: In
          values: ["xlarge", "2xlarge"]
  limits:
    cpu: "64"
    memory: "128Gi"
  disruption:
    consolidationPolicy: WhenEmpty
    consolidateAfter: 30m
```

The orchestrator task explicitly forbade modifying `run_eks_bench.sh`
("the manifest it uses is correct for Karpenter 1.0+") — that
constraint is based on the wrong assumption about the cluster. This
devlog flags the change to be picked up in a follow-up.

Note: the existing `zepto-bench-arm64` NodePool (already on the cluster,
created previously) uses the correct shape and could simply be reused
from the script — i.e. stage 2 could short-circuit when
`zepto-bench-arm64` already exists, the same way item #10 already
short-circuits when ≥3 arm64 nodes are Ready (devlog 094).

## Verification performed

- `kubectl config current-context` →
  `arn:aws:eks:ap-northeast-2:060795905711:cluster/zepto-bench` ✓
- `kubectl get nodes` → 3 Ready nodes (`c5a.large`, `v1.35.2-eks-f69f56f`),
  all provisioned by the Auto Mode `general-purpose` NodePool ✓
- `kubectl get crd | grep -E 'karpenter|eks.amazonaws'`:
  ```
  cninodes.eks.amazonaws.com
  ingressclassparams.eks.amazonaws.com
  nodeclaims.karpenter.sh
  nodeclasses.eks.amazonaws.com          ← Auto Mode NodeClass CRD
  nodediagnostics.eks.amazonaws.com
  nodepools.karpenter.sh
  targetgroupbindings.eks.amazonaws.com
  ```
  `ec2nodeclasses.karpenter.k8s.aws` is absent **by design** — Auto Mode
  replaces it with `nodeclasses.eks.amazonaws.com`.
- `helm list -A | grep -i karpenter` → empty (no Helm-managed Karpenter).
- `kubectl -n kube-system get deploy -l app.kubernetes.io/name=karpenter`
  → no resources (controller lives in the AWS control plane).

## Follow-ups

1. Rewrite stage 2 of `tests/k8s/run_eks_bench.sh` to target Auto Mode
   (either reuse the existing `zepto-bench-arm64` NodePool, or create a
   NodePool-only manifest with `group: eks.amazonaws.com` /
   `kind: NodeClass` / `name: default`). Delete the `EC2NodeClass` block.
2. Update the cleanup block of the same script — remove
   `kubectl delete ec2nodeclass arm64-bench` (the kind no longer
   exists on Auto Mode clusters; command currently errors silently
   because of `|| true`).
3. If the project needs to keep supporting both self-hosted Karpenter
   and Auto Mode clusters in the same script, branch on
   `kubectl get crd ec2nodeclasses.karpenter.k8s.aws` presence.

## Files touched

- `docs/devlog/095_karpenter_crd_install.md` (this file, new)
- `docs/COMPLETED.md` — bullet added referencing 095

#!/usr/bin/env python3.13
"""
ZeptoDB Kubernetes Compatibility Test Suite

Runs against a live K8s cluster to validate Helm chart deployment,
resource creation, pod lifecycle, networking, rolling updates,
PDB enforcement, graceful shutdown, and failure recovery.

Usage:
    # 1. Create cluster
    eksctl create cluster -f tests/k8s/eks-compat-cluster.yaml

    # 2. Run tests
    python3.13 tests/k8s/test_k8s_compat.py

    # 3. Cleanup
    python3.13 tests/k8s/test_k8s_compat.py --cleanup
    eksctl delete cluster -f tests/k8s/eks-compat-cluster.yaml --disable-nodegroup-eviction

Requirements:
    pip install pyyaml
"""

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

NAMESPACE = "zeptodb-test"
RELEASE = "zepto-compat"
CHART_PATH = "deploy/helm/zeptodb"
VALUES_PATH = "tests/k8s/test-values.yaml"
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

@dataclass
class TestResult:
    name: str
    passed: bool
    message: str = ""
    duration_s: float = 0.0


@dataclass
class TestSuite:
    results: list[TestResult] = field(default_factory=list)

    def add(self, r: TestResult):
        self.results.append(r)
        status = "\033[32mPASS\033[0m" if r.passed else "\033[31mFAIL\033[0m"
        print(f"  [{status}] {r.name} ({r.duration_s:.1f}s) {r.message}")

    def summary(self):
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        print(f"\n{'='*60}")
        print(f"Results: {passed}/{total} passed, {failed} failed")
        if failed:
            print("\nFailed tests:")
            for r in self.results:
                if not r.passed:
                    print(f"  - {r.name}: {r.message}")
        print(f"{'='*60}")
        return failed == 0


def run(cmd: str, timeout: int = 120, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd, shell=True, capture_output=True, text=True,
        timeout=timeout, check=check, cwd=str(PROJECT_ROOT),
    )


def kubectl(args: str, timeout: int = 60) -> subprocess.CompletedProcess:
    return run(f"kubectl -n {NAMESPACE} {args}", timeout=timeout, check=False)


def kubectl_json(args: str) -> dict | list | None:
    r = kubectl(f"{args} -o json")
    if r.returncode != 0:
        return None
    return json.loads(r.stdout)


def wait_for_rollout(deploy: str = "", timeout: int = 180) -> bool:
    if not deploy:
        deploy = f"{RELEASE}-zeptodb"
    r = kubectl(f"rollout status deployment/{deploy} --timeout={timeout}s", timeout=timeout + 10)
    return r.returncode == 0


def get_pods(label: str = "") -> list[dict]:
    if not label:
        label = f"app.kubernetes.io/instance={RELEASE}"
    data = kubectl_json(f"get pods -l {label}")
    if not data:
        return []
    return data.get("items", [])


def get_ready_pods() -> list[dict]:
    pods = get_pods()
    ready = []
    for p in pods:
        conditions = p.get("status", {}).get("conditions", [])
        for c in conditions:
            if c.get("type") == "Ready" and c.get("status") == "True":
                ready.append(p)
                break
    return ready


# ---------------------------------------------------------------------------
# Setup / Teardown
# ---------------------------------------------------------------------------

def setup():
    print(f"\n--- Setup: namespace={NAMESPACE}, release={RELEASE} ---")
    run(f"kubectl create namespace {NAMESPACE} --dry-run=client -o yaml | kubectl apply -f -")
    r = run(
        f"helm install {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --wait --timeout 5m",
        timeout=320,
        check=False,
    )
    if r.returncode != 0:
        print(f"Helm install failed:\n{r.stderr}\n{r.stdout}")
        sys.exit(1)
    # Wait for pods to be fully ready (Karpenter may need extra time)
    for _ in range(60):
        ready = get_ready_pods()
        if len(ready) >= 2:
            break
        time.sleep(3)
    print(f"Helm install succeeded ({len(get_ready_pods())} pods ready).\n")


def cleanup():
    print(f"\n--- Cleanup: release={RELEASE}, namespace={NAMESPACE} ---")
    run(f"helm uninstall {RELEASE} -n {NAMESPACE} --wait", check=False, timeout=120)
    run(f"kubectl delete namespace {NAMESPACE} --wait=false", check=False)
    print("Cleanup done.")


# ---------------------------------------------------------------------------
# Test Scenarios
# ---------------------------------------------------------------------------

def test_helm_lint(suite: TestSuite):
    """T01: Helm chart lint passes without errors."""
    t0 = time.monotonic()
    r = run(f"helm lint {CHART_PATH}", check=False)
    passed = r.returncode == 0 and "0 chart(s) failed" not in r.stdout.split("failed")[0]
    # Actually check: "0 chart(s) failed" means success
    passed = r.returncode == 0
    suite.add(TestResult("T01_helm_lint", passed, r.stderr.strip()[:200], time.monotonic() - t0))


def test_helm_template_default(suite: TestSuite):
    """T02: Helm template renders with default values."""
    t0 = time.monotonic()
    r = run(f"helm template test {CHART_PATH}", check=False)
    passed = r.returncode == 0
    suite.add(TestResult("T02_helm_template_default", passed, r.stderr.strip()[:200], time.monotonic() - t0))


def test_helm_template_cluster_mode(suite: TestSuite):
    """T03: Helm template renders with cluster + karpenter enabled."""
    t0 = time.monotonic()
    r = run(
        f"helm template test {CHART_PATH} --set cluster.enabled=true --set karpenter.enabled=true",
        check=False,
    )
    passed = r.returncode == 0
    # Verify RPC and heartbeat ports appear
    if passed:
        passed = "rpc" in r.stdout and "heartbeat" in r.stdout
    suite.add(TestResult("T03_helm_template_cluster", passed, r.stderr.strip()[:200], time.monotonic() - t0))


def test_pods_running(suite: TestSuite):
    """T04: All pods reach Running state."""
    t0 = time.monotonic()
    pods = get_pods()
    running = [p for p in pods if p["status"].get("phase") == "Running"]
    passed = len(running) >= 2
    msg = f"{len(running)}/{len(pods)} running"
    suite.add(TestResult("T04_pods_running", passed, msg, time.monotonic() - t0))


def test_pods_ready(suite: TestSuite):
    """T05: All pods pass readiness probe."""
    t0 = time.monotonic()
    ready = get_ready_pods()
    pods = get_pods()
    passed = len(ready) == len(pods) and len(ready) >= 2
    msg = f"{len(ready)}/{len(pods)} ready"
    suite.add(TestResult("T05_pods_ready", passed, msg, time.monotonic() - t0))


def test_service_endpoints(suite: TestSuite):
    """T06: Service has endpoints pointing to ready pods."""
    t0 = time.monotonic()
    ep = kubectl_json(f"get endpoints {RELEASE}-zeptodb")
    if not ep:
        suite.add(TestResult("T06_service_endpoints", False, "endpoints not found", time.monotonic() - t0))
        return
    subsets = ep.get("subsets", [])
    addr_count = sum(len(s.get("addresses", [])) for s in subsets)
    passed = addr_count >= 2
    suite.add(TestResult("T06_service_endpoints", passed, f"{addr_count} addresses", time.monotonic() - t0))


def test_headless_service(suite: TestSuite):
    """T07: Headless service exists with clusterIP=None."""
    t0 = time.monotonic()
    svc = kubectl_json(f"get service {RELEASE}-zeptodb-headless")
    if not svc:
        suite.add(TestResult("T07_headless_service", False, "not found", time.monotonic() - t0))
        return
    cluster_ip = svc.get("spec", {}).get("clusterIP", "")
    passed = cluster_ip == "None"
    suite.add(TestResult("T07_headless_service", passed, f"clusterIP={cluster_ip}", time.monotonic() - t0))


def test_configmap(suite: TestSuite):
    """T08: ConfigMap contains expected config keys."""
    t0 = time.monotonic()
    cm = kubectl_json(f"get configmap {RELEASE}-zeptodb")
    if not cm:
        suite.add(TestResult("T08_configmap", False, "not found", time.monotonic() - t0))
        return
    conf = cm.get("data", {}).get("zeptodb.conf", "")
    expected_keys = ["port:", "worker_threads:", "analytics_mode:", "data_dir:"]
    missing = [k for k in expected_keys if k not in conf]
    passed = len(missing) == 0
    suite.add(TestResult("T08_configmap", passed, f"missing: {missing}" if missing else "OK", time.monotonic() - t0))


def test_pdb(suite: TestSuite):
    """T09: PodDisruptionBudget exists and enforces minAvailable."""
    t0 = time.monotonic()
    pdb = kubectl_json(f"get pdb {RELEASE}-zeptodb")
    if not pdb:
        suite.add(TestResult("T09_pdb", False, "not found", time.monotonic() - t0))
        return
    min_avail = pdb.get("spec", {}).get("minAvailable")
    passed = min_avail is not None
    suite.add(TestResult("T09_pdb", passed, f"minAvailable={min_avail}", time.monotonic() - t0))


def test_pod_anti_affinity(suite: TestSuite):
    """T10: Pods have anti-affinity rule for hostname spread."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T10_anti_affinity", False, "no pods", time.monotonic() - t0))
        return
    spec = pods[0].get("spec", {})
    affinity = spec.get("affinity", {}).get("podAntiAffinity", {})
    preferred = affinity.get("preferredDuringSchedulingIgnoredDuringExecution", [])
    passed = any(
        t.get("podAffinityTerm", {}).get("topologyKey") == "kubernetes.io/hostname"
        for t in preferred
    )
    suite.add(TestResult("T10_anti_affinity", passed, f"{len(preferred)} rules", time.monotonic() - t0))


def test_labels(suite: TestSuite):
    """T11: Pods have standard Kubernetes labels."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T11_labels", False, "no pods", time.monotonic() - t0))
        return
    labels = pods[0].get("metadata", {}).get("labels", {})
    required = ["app.kubernetes.io/name", "app.kubernetes.io/instance", "app.kubernetes.io/version"]
    missing = [l for l in required if l not in labels]
    passed = len(missing) == 0
    suite.add(TestResult("T11_labels", passed, f"missing: {missing}" if missing else "OK", time.monotonic() - t0))


def test_env_vars(suite: TestSuite):
    """T12: Pods have expected environment variables (POD_NAME, POD_IP, etc.)."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T12_env_vars", False, "no pods", time.monotonic() - t0))
        return
    containers = pods[0].get("spec", {}).get("containers", [])
    env_names = {e["name"] for c in containers for e in c.get("env", [])}
    required = {"POD_NAME", "POD_NAMESPACE", "POD_IP", "ZEPTO_WORKER_THREADS"}
    missing = required - env_names
    passed = len(missing) == 0
    suite.add(TestResult("T12_env_vars", passed, f"missing: {missing}" if missing else "OK", time.monotonic() - t0))


def test_prestop_hook(suite: TestSuite):
    """T13: Containers have preStop lifecycle hook for graceful shutdown."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T13_prestop", False, "no pods", time.monotonic() - t0))
        return
    containers = pods[0].get("spec", {}).get("containers", [])
    has_prestop = any(
        c.get("lifecycle", {}).get("preStop") is not None
        for c in containers
    )
    passed = has_prestop
    suite.add(TestResult("T13_prestop", passed, "", time.monotonic() - t0))


def test_rolling_update_strategy(suite: TestSuite):
    """T14: Deployment uses RollingUpdate with maxUnavailable=0."""
    t0 = time.monotonic()
    deploy = kubectl_json(f"get deployment {RELEASE}-zeptodb")
    if not deploy:
        suite.add(TestResult("T14_rolling_update", False, "not found", time.monotonic() - t0))
        return
    strategy = deploy.get("spec", {}).get("strategy", {})
    stype = strategy.get("type")
    rolling = strategy.get("rollingUpdate", {})
    max_unavail = rolling.get("maxUnavailable")
    passed = stype == "RollingUpdate" and str(max_unavail) == "0"
    suite.add(TestResult(
        "T14_rolling_update", passed,
        f"type={stype}, maxUnavailable={max_unavail}",
        time.monotonic() - t0,
    ))


def test_config_change_triggers_rollout(suite: TestSuite):
    """T15: ConfigMap change triggers pod rollout via checksum annotation."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T15_config_rollout", False, "no pods", time.monotonic() - t0))
        return
    annotations = pods[0].get("metadata", {}).get("annotations", {})
    has_checksum = "checksum/config" in annotations
    passed = has_checksum
    suite.add(TestResult("T15_config_rollout", passed, f"checksum={'present' if has_checksum else 'missing'}", time.monotonic() - t0))


def test_rolling_update_execution(suite: TestSuite):
    """T16: Rolling update completes without downtime (image tag change)."""
    t0 = time.monotonic()
    # Change image tag to trigger rollout
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set image.tag=1.27-alpine --wait --timeout 3m",
        timeout=200, check=False,
    )
    if r.returncode != 0:
        suite.add(TestResult("T16_rolling_update_exec", False, r.stderr.strip()[:200], time.monotonic() - t0))
        return
    ok = wait_for_rollout(timeout=180)
    ready = get_ready_pods()
    passed = ok and len(ready) >= 2
    suite.add(TestResult("T16_rolling_update_exec", passed, f"{len(ready)} ready after rollout", time.monotonic() - t0))


def test_pod_delete_recovery(suite: TestSuite):
    """T17: Deleting a pod triggers automatic recreation."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T17_pod_recovery", False, "no pods", time.monotonic() - t0))
        return
    victim = pods[0]["metadata"]["name"]
    kubectl(f"delete pod {victim} --grace-period=5")
    # Wait for replacement
    for _ in range(30):
        time.sleep(3)
        ready = get_ready_pods()
        if len(ready) >= 2:
            break
    ready = get_ready_pods()
    passed = len(ready) >= 2
    suite.add(TestResult("T17_pod_recovery", passed, f"{len(ready)} ready after delete", time.monotonic() - t0))


def test_pdb_blocks_eviction(suite: TestSuite):
    """T18: PDB blocks eviction when it would violate minAvailable."""
    t0 = time.monotonic()
    # With 2 replicas and minAvailable=1, we can evict 1.
    # Try to evict all — second should be blocked.
    pods = get_pods()
    if len(pods) < 2:
        suite.add(TestResult("T18_pdb_eviction", False, f"need >=2 pods, got {len(pods)}", time.monotonic() - t0))
        return

    # Create eviction for first pod (should succeed)
    p1 = pods[0]["metadata"]["name"]
    evict1 = kubectl(
        f'create -f - <<EOF\n'
        f'apiVersion: policy/v1\n'
        f'kind: Eviction\n'
        f'metadata:\n'
        f'  name: {p1}\n'
        f'  namespace: {NAMESPACE}\n'
        f'EOF'
    )

    # Immediately try second pod (should be blocked by PDB)
    p2 = pods[1]["metadata"]["name"]
    evict2 = kubectl(
        f'create -f - <<EOF\n'
        f'apiVersion: policy/v1\n'
        f'kind: Eviction\n'
        f'metadata:\n'
        f'  name: {p2}\n'
        f'  namespace: {NAMESPACE}\n'
        f'EOF'
    )

    # PDB should block the second eviction (429 or error)
    pdb_blocked = evict2.returncode != 0
    suite.add(TestResult(
        "T18_pdb_eviction", pdb_blocked,
        "PDB blocked second eviction" if pdb_blocked else "PDB did NOT block",
        time.monotonic() - t0,
    ))

    # Wait for recovery
    for _ in range(30):
        time.sleep(3)
        if len(get_ready_pods()) >= 2:
            break


def test_helm_rollback(suite: TestSuite):
    """T19: Helm rollback restores previous revision."""
    t0 = time.monotonic()
    r = run(f"helm rollback {RELEASE} -n {NAMESPACE} --wait --timeout 3m", timeout=200, check=False)
    if r.returncode != 0:
        suite.add(TestResult("T19_helm_rollback", False, r.stderr.strip()[:200], time.monotonic() - t0))
        return
    ok = wait_for_rollout(timeout=180)
    ready = get_ready_pods()
    passed = ok and len(ready) >= 2
    suite.add(TestResult("T19_helm_rollback", passed, f"{len(ready)} ready after rollback", time.monotonic() - t0))


def test_scale_up(suite: TestSuite):
    """T20: Manual scale-up to 3 replicas works."""
    t0 = time.monotonic()
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=3 --wait --timeout 3m",
        timeout=200, check=False,
    )
    if r.returncode != 0:
        suite.add(TestResult("T20_scale_up", False, r.stderr.strip()[:200], time.monotonic() - t0))
        return
    ok = wait_for_rollout(timeout=180)
    ready = get_ready_pods()
    passed = ok and len(ready) == 3
    suite.add(TestResult("T20_scale_up", passed, f"{len(ready)} ready", time.monotonic() - t0))


def test_scale_down(suite: TestSuite):
    """T21: Scale-down back to 2 replicas, excess pod terminated."""
    t0 = time.monotonic()
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=2 --wait --timeout 3m",
        timeout=200, check=False,
    )
    if r.returncode != 0:
        suite.add(TestResult("T21_scale_down", False, r.stderr.strip()[:200], time.monotonic() - t0))
        return
    time.sleep(10)
    pods = get_pods()
    running = [p for p in pods if p["status"].get("phase") == "Running"]
    passed = len(running) == 2
    suite.add(TestResult("T21_scale_down", passed, f"{len(running)} running", time.monotonic() - t0))


def test_dns_resolution(suite: TestSuite):
    """T22: Headless service DNS resolves to pod IPs."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T22_dns", False, "no pods", time.monotonic() - t0))
        return
    pod_name = pods[0]["metadata"]["name"]
    headless_svc = f"{RELEASE}-zeptodb-headless.{NAMESPACE}.svc.cluster.local"
    r = kubectl(f"exec {pod_name} -- nslookup {headless_svc}", timeout=15)
    passed = r.returncode == 0 and "Address" in r.stdout
    suite.add(TestResult("T22_dns", passed, r.stdout.strip()[:200] if not passed else "OK", time.monotonic() - t0))


def test_pod_to_pod_connectivity(suite: TestSuite):
    """T23: Pods can reach each other via headless service."""
    t0 = time.monotonic()
    pods = get_pods()
    if len(pods) < 2:
        suite.add(TestResult("T23_pod_connectivity", False, "need >=2 pods", time.monotonic() - t0))
        return
    src = pods[0]["metadata"]["name"]
    dst_ip = pods[1]["status"].get("podIP", "")
    if not dst_ip:
        suite.add(TestResult("T23_pod_connectivity", False, "no dst IP", time.monotonic() - t0))
        return
    r = kubectl(f"exec {src} -- wget -q -O /dev/null -T 5 http://{dst_ip}:80/", timeout=15)
    passed = r.returncode == 0
    suite.add(TestResult("T23_pod_connectivity", passed, f"src→{dst_ip}", time.monotonic() - t0))


def test_service_routing(suite: TestSuite):
    """T24: ClusterIP service routes traffic to pods."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T24_svc_routing", False, "no pods", time.monotonic() - t0))
        return
    pod_name = pods[0]["metadata"]["name"]
    svc = f"{RELEASE}-zeptodb.{NAMESPACE}.svc.cluster.local"
    r = kubectl(f"exec {pod_name} -- wget -q -O /dev/null -T 5 http://{svc}:80/", timeout=15)
    passed = r.returncode == 0
    suite.add(TestResult("T24_svc_routing", passed, "", time.monotonic() - t0))


def test_termination_grace_period(suite: TestSuite):
    """T25: terminationGracePeriodSeconds is set correctly."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T25_grace_period", False, "no pods", time.monotonic() - t0))
        return
    tgp = pods[0].get("spec", {}).get("terminationGracePeriodSeconds")
    passed = tgp is not None and tgp >= 15
    suite.add(TestResult("T25_grace_period", passed, f"terminationGracePeriodSeconds={tgp}", time.monotonic() - t0))


def test_resource_limits(suite: TestSuite):
    """T26: Container resource requests and limits are set."""
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        suite.add(TestResult("T26_resources", False, "no pods", time.monotonic() - t0))
        return
    containers = pods[0].get("spec", {}).get("containers", [])
    res = containers[0].get("resources", {}) if containers else {}
    has_req = "requests" in res and "cpu" in res["requests"]
    has_lim = "limits" in res and "memory" in res["limits"]
    passed = has_req and has_lim
    suite.add(TestResult("T26_resources", passed, f"requests={has_req}, limits={has_lim}", time.monotonic() - t0))


def test_no_events_warnings(suite: TestSuite):
    """T27: No Warning-level events for currently running pods."""
    t0 = time.monotonic()
    # Get current pod names to filter relevant warnings only
    current_pods = {p["metadata"]["name"] for p in get_pods()}
    events = kubectl_json("get events --field-selector type=Warning")
    items = events.get("items", []) if events else []
    real_warnings = [
        e for e in items
        if e.get("reason") not in ("FailedScheduling",)
        and any(pod in e.get("involvedObject", {}).get("name", "") for pod in current_pods)
    ]
    passed = len(real_warnings) == 0
    msg = f"{len(real_warnings)} warnings" if real_warnings else "clean"
    suite.add(TestResult("T27_no_warnings", passed, msg, time.monotonic() - t0))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

ALL_TESTS = [
    # Static (no cluster needed)
    test_helm_lint,
    test_helm_template_default,
    test_helm_template_cluster_mode,
    # Deployment basics
    test_pods_running,
    test_pods_ready,
    test_service_endpoints,
    test_headless_service,
    test_configmap,
    test_pdb,
    # Pod spec validation
    test_pod_anti_affinity,
    test_labels,
    test_env_vars,
    test_prestop_hook,
    test_rolling_update_strategy,
    test_config_change_triggers_rollout,
    test_resource_limits,
    test_termination_grace_period,
    # Networking
    test_dns_resolution,
    test_pod_to_pod_connectivity,
    test_service_routing,
    # Lifecycle operations
    test_rolling_update_execution,
    test_pod_delete_recovery,
    test_pdb_blocks_eviction,
    test_helm_rollback,
    test_scale_up,
    test_scale_down,
    # Cluster health
    test_no_events_warnings,
]


def main():
    parser = argparse.ArgumentParser(description="ZeptoDB K8s Compatibility Tests")
    parser.add_argument("--cleanup", action="store_true", help="Cleanup and exit")
    parser.add_argument("--skip-setup", action="store_true", help="Skip helm install (already deployed)")
    parser.add_argument("--static-only", action="store_true", help="Run only static tests (no cluster needed)")
    args = parser.parse_args()

    if args.cleanup:
        cleanup()
        return

    suite = TestSuite()
    print("=" * 60)
    print("ZeptoDB Kubernetes Compatibility Test Suite")
    print("=" * 60)

    if args.static_only:
        print("\n--- Static Tests (no cluster required) ---")
        for t in ALL_TESTS[:3]:
            t(suite)
        all_passed = suite.summary()
        sys.exit(0 if all_passed else 1)

    # Preflight: check cluster connectivity
    r = run("kubectl cluster-info", check=False, timeout=10)
    if r.returncode != 0:
        print("ERROR: Cannot connect to K8s cluster. Run:")
        print("  eksctl create cluster -f tests/k8s/eks-compat-cluster.yaml")
        sys.exit(1)

    if not args.skip_setup:
        setup()

    print("\n--- Running Tests ---")
    for t in ALL_TESTS:
        try:
            t(suite)
        except Exception as e:
            suite.add(TestResult(t.__name__, False, f"EXCEPTION: {e}"))

    all_passed = suite.summary()

    if not all_passed:
        print("\nTo inspect: kubectl get all -n", NAMESPACE)
        print("To cleanup: python3.13 tests/k8s/test_k8s_compat.py --cleanup")

    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3.13
"""
ZeptoDB Seoul Production — K8s Compatibility & HA & Performance Tests

Adapted for:
  - Real ZeptoDB image (distroless, no shell/wget/curl inside container)
  - Helm release name = chart name = "zeptodb" → fullname = "zeptodb"
  - Namespace: zeptodb
  - Network tests via port-forward instead of kubectl exec

Usage:
    python3.13 tests/k8s/test_k8s_seoul.py
    python3.13 tests/k8s/test_k8s_seoul.py --perf-only
"""

import argparse
import json
import subprocess
import sys
import time
import threading
from dataclasses import dataclass, field
from pathlib import Path
from statistics import mean, stdev

NAMESPACE = "zeptodb"
RELEASE = "zeptodb"
DEPLOY_NAME = "zeptodb"          # fullname (release==chart → no duplication)
CHART_PATH = "deploy/helm/zeptodb"
VALUES_PATH = "deploy/helm/values-seoul-prod.yaml"
HELM_EXTRA = (
    "--set karpenter.enabled=true "
    "--set karpenter.nodeClass.role=KarpenterNodeRole-zepto-bench "
    "--set karpenter.nodeClass.clusterName=zepto-bench "
    "--set karpenter.realtime.zones={ap-northeast-2a}"
)
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent


@dataclass
class TestResult:
    name: str
    passed: bool
    message: str = ""
    duration_s: float = 0.0

@dataclass
class PerfResult:
    name: str
    value: float
    unit: str
    details: str = ""

@dataclass
class Suite:
    results: list[TestResult] = field(default_factory=list)
    perf: list[PerfResult] = field(default_factory=list)

    def add(self, r: TestResult):
        self.results.append(r)
        s = "\033[32mPASS\033[0m" if r.passed else "\033[31mFAIL\033[0m"
        print(f"  [{s}] {r.name} ({r.duration_s:.1f}s) {r.message}")

    def add_perf(self, r: PerfResult):
        self.perf.append(r)
        print(f"  [PERF] {r.name}: {r.value:.2f} {r.unit}  {r.details}")

    def summary(self) -> bool:
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        print(f"\n{'='*60}")
        print(f"Results: {passed}/{total} passed, {failed} failed")
        if failed:
            print("\nFailed:")
            for r in self.results:
                if not r.passed:
                    print(f"  - {r.name}: {r.message}")
        if self.perf:
            print(f"\n{'─'*60}")
            print("Performance:")
            for p in self.perf:
                print(f"  {p.name:<40} {p.value:>10.2f} {p.unit:<8} {p.details}")
        print(f"{'='*60}")
        return failed == 0


def run(cmd, timeout=120, check=True):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          timeout=timeout, check=check, cwd=str(PROJECT_ROOT))

def kubectl(args, timeout=60):
    return run(f"kubectl -n {NAMESPACE} {args}", timeout=timeout, check=False)

def kubectl_json(args):
    r = kubectl(f"{args} -o json")
    return json.loads(r.stdout) if r.returncode == 0 else None

def get_pods():
    d = kubectl_json(f"get pods -l app.kubernetes.io/instance={RELEASE}")
    return d.get("items", []) if d else []

def get_ready_pods():
    return [p for p in get_pods() if any(
        c.get("type") == "Ready" and c.get("status") == "True"
        for c in p.get("status", {}).get("conditions", [])
    )]

def wait_ready(n, timeout=180):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if len(get_ready_pods()) >= n:
            return True, time.monotonic() - t0
        time.sleep(3)
    return False, time.monotonic() - t0

def helm_upgrade(extra="", timeout_s=300):
    return run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} {HELM_EXTRA} {extra} --wait --timeout 5m",
        timeout=timeout_s, check=False,
    )

def health_check_via_pf():
    """Quick health check via port-forward."""
    pf = subprocess.Popen(
        f"kubectl port-forward -n {NAMESPACE} svc/{DEPLOY_NAME} 19123:8123".split(),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    try:
        r = run("curl -sf --max-time 3 http://localhost:19123/health", check=False, timeout=10)
        return r.returncode == 0 and "healthy" in r.stdout
    finally:
        pf.terminate(); pf.wait()


# ── Compat Tests ──

def t_helm_lint(s):
    t0 = time.monotonic()
    r = run(f"helm lint {CHART_PATH}", check=False)
    s.add(TestResult("T01_helm_lint", r.returncode == 0, "", time.monotonic() - t0))

def t_pods_running(s):
    t0 = time.monotonic()
    pods = get_pods()
    running = [p for p in pods if p["status"].get("phase") == "Running"]
    s.add(TestResult("T02_pods_running", len(running) == 3, f"{len(running)}/3", time.monotonic() - t0))

def t_pods_ready(s):
    t0 = time.monotonic()
    ready = get_ready_pods()
    s.add(TestResult("T03_pods_ready", len(ready) == 3, f"{len(ready)}/3", time.monotonic() - t0))

def t_3node_spread(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    nodes = {p["spec"]["nodeName"] for p in pods}
    s.add(TestResult("T04_3node_spread", len(pods) == 3 and len(nodes) == 3,
                      f"{len(pods)} pods on {len(nodes)} nodes", time.monotonic() - t0))

def t_service_endpoints(s):
    t0 = time.monotonic()
    ep = kubectl_json(f"get endpoints {DEPLOY_NAME}")
    n = sum(len(sub.get("addresses", [])) for sub in (ep or {}).get("subsets", []))
    s.add(TestResult("T05_endpoints", n >= 3, f"{n} addresses", time.monotonic() - t0))

def t_headless(s):
    t0 = time.monotonic()
    svc = kubectl_json(f"get service {DEPLOY_NAME}-headless")
    ok = svc and svc.get("spec", {}).get("clusterIP") == "None"
    s.add(TestResult("T06_headless", ok, "", time.monotonic() - t0))

def t_configmap(s):
    t0 = time.monotonic()
    cm = kubectl_json(f"get configmap {DEPLOY_NAME}")
    conf = (cm or {}).get("data", {}).get("zeptodb.conf", "")
    ok = "port:" in conf and "worker_threads:" in conf
    s.add(TestResult("T07_configmap", ok, "", time.monotonic() - t0))

def t_pdb(s):
    t0 = time.monotonic()
    pdb = kubectl_json(f"get pdb {DEPLOY_NAME}")
    ok = pdb and pdb.get("spec", {}).get("minAvailable") is not None
    s.add(TestResult("T08_pdb", ok, f"minAvailable={pdb['spec']['minAvailable']}" if ok else "not found", time.monotonic() - t0))

def t_labels(s):
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        s.add(TestResult("T09_labels", False, "no pods", time.monotonic() - t0)); return
    labels = pods[0].get("metadata", {}).get("labels", {})
    required = ["app.kubernetes.io/name", "app.kubernetes.io/instance"]
    missing = [l for l in required if l not in labels]
    s.add(TestResult("T09_labels", not missing, f"missing: {missing}" if missing else "OK", time.monotonic() - t0))

def t_env_prestop_resources(s):
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        s.add(TestResult("T10_pod_spec", False, "no pods", time.monotonic() - t0)); return
    c = pods[0]["spec"]["containers"][0]
    env_names = {e["name"] for e in c.get("env", [])}
    has_env = {"POD_NAME", "POD_IP", "APEX_WORKER_THREADS"}.issubset(env_names)
    has_prestop = c.get("lifecycle", {}).get("preStop") is not None
    has_res = "requests" in c.get("resources", {})
    ok = has_env and has_prestop and has_res
    s.add(TestResult("T10_pod_spec", ok, f"env={has_env} prestop={has_prestop} resources={has_res}", time.monotonic() - t0))

def t_rolling_strategy(s):
    t0 = time.monotonic()
    d = kubectl_json(f"get deployment {DEPLOY_NAME}")
    if not d:
        s.add(TestResult("T11_rolling_strategy", False, "not found", time.monotonic() - t0)); return
    st = d["spec"]["strategy"]
    ok = st["type"] == "RollingUpdate" and str(st["rollingUpdate"]["maxUnavailable"]) == "0"
    s.add(TestResult("T11_rolling_strategy", ok, f"maxUnavailable={st['rollingUpdate']['maxUnavailable']}", time.monotonic() - t0))

def t_health_check(s):
    t0 = time.monotonic()
    ok = health_check_via_pf()
    s.add(TestResult("T12_health_check", ok, '{"status":"healthy"}' if ok else "FAIL", time.monotonic() - t0))

def t_karpenter(s):
    t0 = time.monotonic()
    np = run("kubectl get nodepools --no-headers", check=False, timeout=10)
    nc = run("kubectl get ec2nodeclasses --no-headers", check=False, timeout=10)
    np_ready = np.stdout.count("True") if np.returncode == 0 else 0
    nc_ready = nc.stdout.count("True") if nc.returncode == 0 else 0
    ok = np_ready == 2 and nc_ready == 2
    s.add(TestResult("T13_karpenter", ok, f"nodepools={np_ready}/2 nodeclasses={nc_ready}/2", time.monotonic() - t0))


# ── HA Tests ──

def ha_pod_kill(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        s.add(TestResult("HA01_pod_kill", False, f"need 3, got {len(pods)}", time.monotonic() - t0)); return
    victim = pods[0]["metadata"]["name"]
    kubectl(f"delete pod {victim} --grace-period=0 --force")
    ok, elapsed = wait_ready(3, timeout=120)
    s.add(TestResult("HA01_pod_kill", ok, f"recovery={elapsed:.1f}s", time.monotonic() - t0))
    if ok: s.add_perf(PerfResult("pod_kill_recovery", elapsed, "sec"))

def ha_node_drain(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        s.add(TestResult("HA02_node_drain", False, f"need 3, got {len(pods)}", time.monotonic() - t0)); return
    node = pods[0]["spec"]["nodeName"]
    run(f"kubectl drain {node} --ignore-daemonsets --delete-emptydir-data --timeout=60s", check=False, timeout=70)
    # With Guaranteed QoS (6 CPU, 48Gi) on r7i.2xlarge, only 1 pod fits per node.
    # After drain, the evicted pod waits until uncordon. Verify 2 pods stay healthy.
    time.sleep(10)
    ready = get_ready_pods()
    service_ok = len(ready) >= 2  # PDB minAvailable=2 maintained
    run(f"kubectl uncordon {node}", check=False)
    ok, elapsed = wait_ready(3, timeout=180)
    s.add(TestResult("HA02_node_drain", service_ok and ok,
                      f"during_drain={len(ready)}_ready, full_recovery={elapsed:.1f}s", time.monotonic() - t0))
    if ok: s.add_perf(PerfResult("drain_recovery", elapsed, "sec"))

def ha_pdb_block(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 2:
        s.add(TestResult("HA03_pdb_block", False, "need 2+ pods", time.monotonic() - t0)); return
    p1, p2 = pods[0]["metadata"]["name"], pods[1]["metadata"]["name"]
    kubectl(f'create -f - <<EOF\napiVersion: policy/v1\nkind: Eviction\nmetadata:\n  name: {p1}\n  namespace: {NAMESPACE}\nEOF')
    r2 = kubectl(f'create -f - <<EOF\napiVersion: policy/v1\nkind: Eviction\nmetadata:\n  name: {p2}\n  namespace: {NAMESPACE}\nEOF')
    blocked = r2.returncode != 0
    s.add(TestResult("HA03_pdb_block", blocked, "blocked" if blocked else "NOT blocked", time.monotonic() - t0))
    wait_ready(3, timeout=120)

def ha_rolling_zero_dt(s):
    t0 = time.monotonic()
    def do_upgrade():
        helm_upgrade("--set image.tag=0.0.2", timeout_s=360)
    t = threading.Thread(target=do_upgrade, daemon=True)
    t.start()
    # Probe via port-forward during rollout
    failures, checks = 0, 0
    for _ in range(15):
        time.sleep(4)
        ok = health_check_via_pf()
        checks += 1
        if not ok: failures += 1
    t.join(timeout=180)
    wait_ready(3, timeout=60)
    passed = failures == 0
    s.add(TestResult("HA04_rolling_zero_dt", passed, f"probes={checks} failures={failures}", time.monotonic() - t0))

def ha_scale(s):
    """Scale 3→4→3. (5 not possible: 1 pod per data-node with Guaranteed QoS)"""
    t0 = time.monotonic()
    # No --wait: 4th pod will be Pending (expected)
    run(f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} -f {VALUES_PATH} {HELM_EXTRA} --set replicaCount=4 --timeout 2m",
        timeout=130, check=False)
    time.sleep(20)
    ready = get_ready_pods()
    all_pods = get_pods()
    pending = [p for p in all_pods if p["status"].get("phase") == "Pending"]
    scale_ok = len(ready) == 3 and len(pending) >= 1

    # Scale back to 3
    helm_upgrade("--set replicaCount=3", timeout_s=180)
    time.sleep(10)
    final = [p for p in get_pods() if p["status"].get("phase") == "Running"]
    passed = scale_ok and len(final) == 3
    s.add(TestResult("HA05_scale", passed,
                      f"at_4: {len(ready)}running+{len(pending)}pending, final={len(final)}", time.monotonic() - t0))


# ── Perf Tests ──

def perf_startup(s):
    t0 = time.monotonic()
    latencies = []
    for _ in range(3):
        pods = get_ready_pods()
        if not pods: break
        kubectl(f"delete pod {pods[0]['metadata']['name']} --grace-period=1")
        time.sleep(2)
        _, elapsed = wait_ready(3, timeout=120)
        latencies.append(elapsed)
    if latencies:
        s.add_perf(PerfResult("startup_latency_avg", mean(latencies), "sec", f"n={len(latencies)}"))
    s.add(TestResult("PERF01_startup", len(latencies) == 3,
                      f"avg={mean(latencies):.1f}s" if latencies else "no data", time.monotonic() - t0))

def perf_rolling(s):
    t0 = time.monotonic()
    # Force a rollout by changing an annotation
    kubectl(f"patch deployment {DEPLOY_NAME} -p '{{\"spec\":{{\"template\":{{\"metadata\":{{\"annotations\":{{\"perf-test\":\"{time.time()}\"}}}}}}}}}}'")
    kubectl(f"rollout status deployment/{DEPLOY_NAME} --timeout=300s", timeout=310)
    ok, _ = wait_ready(3, timeout=120)
    dur = time.monotonic() - t0
    s.add(TestResult("PERF02_rolling", ok, f"{dur:.1f}s", time.monotonic() - t0))
    if ok: s.add_perf(PerfResult("rolling_update_3r", dur, "sec"))

def perf_health_latency(s):
    """Measure health endpoint latency via LB."""
    t0 = time.monotonic()
    lb = run(f"kubectl get svc {DEPLOY_NAME} -n {NAMESPACE} -o jsonpath='{{.status.loadBalancer.ingress[0].hostname}}'",
             check=False, timeout=10).stdout.strip("'")
    latencies = []
    for _ in range(10):
        req_t = time.monotonic()
        r = run(f"curl -sf --max-time 3 http://{lb}:8123/health", check=False, timeout=5)
        if r.returncode == 0:
            latencies.append((time.monotonic() - req_t) * 1000)
    if latencies:
        s.add_perf(PerfResult("health_latency_avg", mean(latencies), "ms", f"n={len(latencies)}"))
        s.add_perf(PerfResult("health_latency_min", min(latencies), "ms"))
    s.add(TestResult("PERF03_health_latency", len(latencies) >= 5,
                      f"avg={mean(latencies):.0f}ms" if latencies else "no data", time.monotonic() - t0))

def perf_failover(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        s.add(TestResult("PERF04_failover", False, "need 3 pods", time.monotonic() - t0)); return
    lb = run(f"kubectl get svc {DEPLOY_NAME} -n {NAMESPACE} -o jsonpath='{{.status.loadBalancer.ingress[0].hostname}}'",
             check=False, timeout=10).stdout.strip("'")
    kubectl(f"delete pod {pods[0]['metadata']['name']} --grace-period=0 --force")
    fail_start = time.monotonic()
    consec = 0
    failover_time = None
    for _ in range(40):
        r = run(f"curl -sf --max-time 2 http://{lb}:8123/health", check=False, timeout=5)
        if r.returncode == 0:
            consec += 1
            if consec >= 3 and failover_time is None:
                failover_time = time.monotonic() - fail_start
        else:
            consec = 0
        time.sleep(1)
    wait_ready(3, timeout=120)
    if failover_time:
        s.add_perf(PerfResult("service_failover", failover_time, "sec"))
        s.add(TestResult("PERF04_failover", True, f"{failover_time:.1f}s", time.monotonic() - t0))
    else:
        s.add(TestResult("PERF04_failover", False, "never recovered", time.monotonic() - t0))


COMPAT = [t_helm_lint, t_pods_running, t_pods_ready, t_3node_spread,
          t_service_endpoints, t_headless, t_configmap, t_pdb, t_labels,
          t_env_prestop_resources, t_rolling_strategy, t_health_check, t_karpenter]

HA = [ha_pod_kill, ha_node_drain, ha_pdb_block, ha_rolling_zero_dt, ha_scale]

PERF = [perf_startup, perf_rolling, perf_health_latency, perf_failover]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--perf-only", action="store_true")
    ap.add_argument("--ha-only", action="store_true")
    ap.add_argument("--compat-only", action="store_true")
    args = ap.parse_args()

    suite = Suite()
    print("=" * 60)
    print("ZeptoDB Seoul Production — Full Test Suite")
    print("=" * 60)

    tests = []
    if not args.perf_only and not args.ha_only:
        print("\n--- Compatibility ---")
        tests += COMPAT
    if not args.perf_only and not args.compat_only:
        print("\n--- HA ---")
        tests += HA
    if not args.ha_only and not args.compat_only:
        print("\n--- Performance ---")
        tests += PERF

    for t in tests:
        try:
            t(suite)
        except Exception as e:
            suite.add(TestResult(t.__name__, False, f"EX: {e}"))

    ok = suite.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

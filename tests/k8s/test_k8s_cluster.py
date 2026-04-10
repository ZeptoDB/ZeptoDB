#!/usr/bin/env python3.13
"""
ZeptoDB Seoul Production — Cluster-Based Full Test Suite

Tests StatefulSet cluster deployment: compatibility, HA, distributed queries,
and performance benchmarks against a real 3-node ZeptoDB cluster.

Usage:
    python3.13 tests/k8s/test_k8s_cluster.py
    python3.13 tests/k8s/test_k8s_cluster.py --compat-only
    python3.13 tests/k8s/test_k8s_cluster.py --ha-only
    python3.13 tests/k8s/test_k8s_cluster.py --perf-only
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
STS_NAME = "zeptodb"
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

def get_lb():
    r = run(f"kubectl get svc {STS_NAME} -n {NAMESPACE} -o jsonpath='{{.status.loadBalancer.ingress[0].hostname}}'",
            check=False, timeout=10)
    return r.stdout.strip("'")

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

def curl_lb(path, method="GET", data=None, timeout=5):
    lb = get_lb()
    cmd = f"curl -sf --max-time {timeout} "
    if method == "POST":
        cmd += f"-X POST http://{lb}:8123{path}"
        if data:
            cmd += f" -d '{data}'"
    else:
        cmd += f"http://{lb}:8123{path}"
    r = run(cmd, check=False, timeout=timeout + 5)
    return r

def curl_lb_json(path, method="GET", data=None):
    r = curl_lb(path, method, data)
    if r.returncode == 0 and r.stdout:
        try:
            return json.loads(r.stdout)
        except json.JSONDecodeError:
            pass
    return None


# ═══════════════════════════════════════════════════════════
# Compatibility Tests
# ═══════════════════════════════════════════════════════════

def t_helm_lint(s):
    t0 = time.monotonic()
    r = run(f"helm lint {CHART_PATH}", check=False)
    s.add(TestResult("C01_helm_lint", r.returncode == 0, "", time.monotonic() - t0))

def t_statefulset_exists(s):
    t0 = time.monotonic()
    sts = kubectl_json(f"get statefulset {STS_NAME}")
    ok = sts is not None and sts.get("spec", {}).get("replicas") == 3
    s.add(TestResult("C02_statefulset", ok,
                      f"replicas={sts['spec']['replicas']}" if ok else "not found", time.monotonic() - t0))

def t_pods_running(s):
    t0 = time.monotonic()
    pods = get_pods()
    running = [p for p in pods if p["status"].get("phase") == "Running"]
    s.add(TestResult("C03_pods_running", len(running) == 3, f"{len(running)}/3", time.monotonic() - t0))

def t_pods_ready(s):
    t0 = time.monotonic()
    ready = get_ready_pods()
    s.add(TestResult("C04_pods_ready", len(ready) == 3, f"{len(ready)}/3", time.monotonic() - t0))

def t_3node_spread(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    nodes = {p["spec"]["nodeName"] for p in pods}
    s.add(TestResult("C05_3node_spread", len(pods) == 3 and len(nodes) == 3,
                      f"{len(pods)} pods on {len(nodes)} nodes", time.monotonic() - t0))

def t_stable_hostnames(s):
    """StatefulSet pods have stable hostnames: zeptodb-0, zeptodb-1, zeptodb-2."""
    t0 = time.monotonic()
    pods = get_pods()
    names = sorted(p["metadata"]["name"] for p in pods)
    expected = [f"{STS_NAME}-0", f"{STS_NAME}-1", f"{STS_NAME}-2"]
    s.add(TestResult("C06_stable_hostnames", names == expected,
                      f"{names}", time.monotonic() - t0))

def t_headless_service(s):
    t0 = time.monotonic()
    svc = kubectl_json(f"get service {STS_NAME}-headless")
    ok = svc and svc.get("spec", {}).get("clusterIP") == "None"
    s.add(TestResult("C07_headless", ok, "", time.monotonic() - t0))

def t_endpoints(s):
    t0 = time.monotonic()
    ep = kubectl_json(f"get endpoints {STS_NAME}")
    n = sum(len(sub.get("addresses", [])) for sub in (ep or {}).get("subsets", []))
    s.add(TestResult("C08_endpoints", n >= 3, f"{n} addresses", time.monotonic() - t0))

def t_pdb(s):
    t0 = time.monotonic()
    pdb = kubectl_json(f"get pdb {STS_NAME}")
    ok = pdb and pdb.get("spec", {}).get("minAvailable") is not None
    s.add(TestResult("C09_pdb", ok,
                      f"minAvailable={pdb['spec']['minAvailable']}" if ok else "not found", time.monotonic() - t0))

def t_pod_spec(s):
    t0 = time.monotonic()
    pods = get_pods()
    if not pods:
        s.add(TestResult("C10_pod_spec", False, "no pods", time.monotonic() - t0)); return
    c = pods[0]["spec"]["containers"][0]
    env_names = {e["name"] for e in c.get("env", [])}
    has_env = {"POD_NAME", "POD_IP", "APEX_WORKER_THREADS"}.issubset(env_names)
    has_prestop = c.get("lifecycle", {}).get("preStop") is not None
    has_res = "requests" in c.get("resources", {})
    ok = has_env and has_prestop and has_res
    s.add(TestResult("C10_pod_spec", ok,
                      f"env={has_env} prestop={has_prestop} resources={has_res}", time.monotonic() - t0))

def t_health(s):
    t0 = time.monotonic()
    r = curl_lb("/health")
    ok = r.returncode == 0 and "healthy" in r.stdout
    s.add(TestResult("C11_health", ok, r.stdout.strip()[:60], time.monotonic() - t0))

def t_karpenter(s):
    t0 = time.monotonic()
    np = run("kubectl get nodepools --no-headers", check=False, timeout=10)
    nc = run("kubectl get ec2nodeclasses --no-headers", check=False, timeout=10)
    np_ready = np.stdout.count("True") if np.returncode == 0 else 0
    nc_ready = nc.stdout.count("True") if nc.returncode == 0 else 0
    ok = np_ready == 2 and nc_ready == 2
    s.add(TestResult("C12_karpenter", ok, f"nodepools={np_ready}/2 nodeclasses={nc_ready}/2", time.monotonic() - t0))


# ═══════════════════════════════════════════════════════════
# Cluster / Distributed Query Tests
# ═══════════════════════════════════════════════════════════

def d_cluster_mode(s):
    t0 = time.monotonic()
    data = curl_lb_json("/admin/cluster")
    if not data:
        s.add(TestResult("D01_cluster_mode", False, "no response", time.monotonic() - t0)); return
    mode = data.get("mode")
    node_count = data.get("node_count")
    ok = mode == "cluster" and node_count == 3
    s.add(TestResult("D01_cluster_mode", ok, f"mode={mode} nodes={node_count}", time.monotonic() - t0))

def d_query_count(s):
    t0 = time.monotonic()
    data = curl_lb_json("/", "POST", "SELECT count(*) FROM trades")
    if not data:
        s.add(TestResult("D02_query_count", False, "no response", time.monotonic() - t0)); return
    rows = data.get("data", [[]])[0]
    count = rows[0] if rows else 0
    ok = count > 0
    s.add(TestResult("D02_query_count", ok, f"count={count}", time.monotonic() - t0))

def d_query_groupby(s):
    t0 = time.monotonic()
    data = curl_lb_json("/", "POST", "SELECT symbol, count(*) FROM trades GROUP BY symbol")
    if not data:
        s.add(TestResult("D03_query_groupby", False, "no response", time.monotonic() - t0)); return
    nrows = data.get("rows", 0)
    ok = nrows >= 3
    s.add(TestResult("D03_query_groupby", ok, f"groups={nrows}", time.monotonic() - t0))

def d_query_vwap(s):
    t0 = time.monotonic()
    data = curl_lb_json("/", "POST", "SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol")
    if not data:
        s.add(TestResult("D04_query_vwap", False, "no response", time.monotonic() - t0)); return
    ok = data.get("rows", 0) >= 3
    exec_us = data.get("execution_time_us", 0)
    s.add(TestResult("D04_query_vwap", ok, f"rows={data['rows']} exec={exec_us}μs", time.monotonic() - t0))
    if ok:
        s.add_perf(PerfResult("vwap_query_latency", exec_us, "μs", f"{data['rows']} groups"))

def d_query_all_pods(s):
    """Query each pod directly to verify all have data."""
    t0 = time.monotonic()
    counts = []
    for i in range(3):
        pf = subprocess.Popen(
            f"kubectl port-forward -n {NAMESPACE} pod/{STS_NAME}-{i} 2{i}123:8123".split(),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)
        try:
            r = run(f"curl -sf --max-time 3 -X POST http://localhost:2{i}123/ -d 'SELECT count(*) FROM trades'",
                    check=False, timeout=8)
            if r.returncode == 0:
                d = json.loads(r.stdout)
                counts.append(d["data"][0][0])
        finally:
            pf.terminate(); pf.wait()
    ok = len(counts) == 3 and all(c > 0 for c in counts)
    s.add(TestResult("D05_all_pods_have_data", ok, f"counts={counts}", time.monotonic() - t0))

def d_consistent_results(s):
    """Same query to different pods returns same result."""
    t0 = time.monotonic()
    results = []
    for _ in range(5):
        data = curl_lb_json("/", "POST", "SELECT count(*) FROM trades")
        if data:
            results.append(data["data"][0][0])
    ok = len(set(results)) == 1 and len(results) == 5
    s.add(TestResult("D06_consistent_results", ok, f"results={results}", time.monotonic() - t0))


# ═══════════════════════════════════════════════════════════
# HA Tests
# ═══════════════════════════════════════════════════════════

def ha_pod_kill(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        s.add(TestResult("HA01_pod_kill", False, f"need 3, got {len(pods)}", time.monotonic() - t0)); return
    kubectl(f"delete pod {STS_NAME}-0 --grace-period=0 --force")
    ok, elapsed = wait_ready(3, timeout=120)
    # Verify cluster still responds
    data = curl_lb_json("/admin/cluster")
    mode = data.get("mode") if data else "unknown"
    s.add(TestResult("HA01_pod_kill", ok and mode == "cluster",
                      f"recovery={elapsed:.1f}s mode={mode}", time.monotonic() - t0))
    if ok: s.add_perf(PerfResult("pod_kill_recovery", elapsed, "sec"))

def ha_node_drain(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        s.add(TestResult("HA02_node_drain", False, f"need 3, got {len(pods)}", time.monotonic() - t0)); return
    node = pods[0]["spec"]["nodeName"]
    run(f"kubectl drain {node} --ignore-daemonsets --delete-emptydir-data --timeout=60s", check=False, timeout=70)
    time.sleep(10)
    ready = get_ready_pods()
    service_ok = len(ready) >= 2
    # Verify queries still work during drain
    data = curl_lb_json("/", "POST", "SELECT count(*) FROM trades")
    query_ok = data is not None and data.get("data", [[0]])[0][0] > 0
    run(f"kubectl uncordon {node}", check=False)
    ok_full, elapsed = wait_ready(3, timeout=180)
    passed = service_ok and query_ok and ok_full
    s.add(TestResult("HA02_node_drain", passed,
                      f"during={len(ready)}ready query={'OK' if query_ok else 'FAIL'} full={elapsed:.1f}s",
                      time.monotonic() - t0))
    if ok_full: s.add_perf(PerfResult("drain_recovery", elapsed, "sec"))

def ha_pdb_block(s):
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 2:
        s.add(TestResult("HA03_pdb_block", False, "need 2+ pods", time.monotonic() - t0)); return
    kubectl(f'create -f - <<EOF\napiVersion: policy/v1\nkind: Eviction\nmetadata:\n  name: {STS_NAME}-0\n  namespace: {NAMESPACE}\nEOF')
    r2 = kubectl(f'create -f - <<EOF\napiVersion: policy/v1\nkind: Eviction\nmetadata:\n  name: {STS_NAME}-1\n  namespace: {NAMESPACE}\nEOF')
    blocked = r2.returncode != 0
    s.add(TestResult("HA03_pdb_block", blocked, "blocked" if blocked else "NOT blocked", time.monotonic() - t0))
    wait_ready(3, timeout=120)

def ha_rolling_zero_dt(s):
    t0 = time.monotonic()
    def do_rollout():
        kubectl(f"patch statefulset {STS_NAME} -p '{{\"spec\":{{\"template\":{{\"metadata\":{{\"annotations\":{{\"ha-test\":\"{time.time()}\"}}}}}}}}}}'")
        kubectl(f"rollout status statefulset/{STS_NAME} --timeout=300s", timeout=310)
    t = threading.Thread(target=do_rollout, daemon=True)
    t.start()
    failures, checks = 0, 0
    for _ in range(20):
        time.sleep(3)
        r = curl_lb("/health")
        checks += 1
        if r.returncode != 0 or "healthy" not in r.stdout:
            failures += 1
    t.join(timeout=180)
    wait_ready(3, timeout=60)
    s.add(TestResult("HA04_rolling_zero_dt", failures == 0,
                      f"probes={checks} failures={failures}", time.monotonic() - t0))

def ha_query_during_restart(s):
    """Queries return valid results while a pod is restarting."""
    t0 = time.monotonic()
    kubectl(f"delete pod {STS_NAME}-2 --grace-period=0 --force")
    successes, failures = 0, 0
    for _ in range(15):
        data = curl_lb_json("/", "POST", "SELECT count(*) FROM trades")
        if data and data.get("data", [[0]])[0][0] > 0:
            successes += 1
        else:
            failures += 1
        time.sleep(2)
    wait_ready(3, timeout=120)
    s.add(TestResult("HA05_query_during_restart", successes > 0,
                      f"ok={successes} fail={failures}", time.monotonic() - t0))


# ═══════════════════════════════════════════════════════════
# Performance Benchmarks
# ═══════════════════════════════════════════════════════════

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
    s.add(TestResult("P01_startup", len(latencies) == 3,
                      f"avg={mean(latencies):.1f}s" if latencies else "no data", time.monotonic() - t0))

def perf_health_latency(s):
    t0 = time.monotonic()
    latencies = []
    for _ in range(20):
        req_t = time.monotonic()
        r = curl_lb("/health")
        if r.returncode == 0:
            latencies.append((time.monotonic() - req_t) * 1000)
    if latencies:
        s.add_perf(PerfResult("health_latency_avg", mean(latencies), "ms", f"n={len(latencies)}"))
        s.add_perf(PerfResult("health_latency_min", min(latencies), "ms"))
    s.add(TestResult("P02_health_latency", len(latencies) >= 10,
                      f"avg={mean(latencies):.0f}ms" if latencies else "no data", time.monotonic() - t0))

def perf_query_latency(s):
    """Measure query latency for different query types."""
    t0 = time.monotonic()
    queries = [
        ("count", "SELECT count(*) FROM trades"),
        ("groupby", "SELECT symbol, count(*) FROM trades GROUP BY symbol"),
        ("vwap", "SELECT symbol, vwap(price, volume) FROM trades GROUP BY symbol"),
    ]
    for name, sql in queries:
        times = []
        for _ in range(10):
            data = curl_lb_json("/", "POST", sql)
            if data and "execution_time_us" in data:
                times.append(data["execution_time_us"])
        if times:
            s.add_perf(PerfResult(f"query_{name}_avg", mean(times), "μs", f"n={len(times)}"))
            s.add_perf(PerfResult(f"query_{name}_min", min(times), "μs"))
    s.add(TestResult("P03_query_latency", True, "see perf results", time.monotonic() - t0))

def perf_failover(s):
    t0 = time.monotonic()
    kubectl(f"delete pod {STS_NAME}-0 --grace-period=0 --force")
    fail_start = time.monotonic()
    consec, failover_time = 0, None
    for _ in range(40):
        r = curl_lb("/health")
        if r.returncode == 0 and "healthy" in r.stdout:
            consec += 1
            if consec >= 3 and failover_time is None:
                failover_time = time.monotonic() - fail_start
        else:
            consec = 0
        time.sleep(1)
    wait_ready(3, timeout=120)
    if failover_time:
        s.add_perf(PerfResult("service_failover", failover_time, "sec"))
        s.add(TestResult("P04_failover", True, f"{failover_time:.1f}s", time.monotonic() - t0))
    else:
        s.add(TestResult("P04_failover", False, "never recovered", time.monotonic() - t0))


# ═══════════════════════════════════════════════════════════

COMPAT = [t_helm_lint, t_statefulset_exists, t_pods_running, t_pods_ready,
          t_3node_spread, t_stable_hostnames, t_headless_service, t_endpoints,
          t_pdb, t_pod_spec, t_health, t_karpenter]

DISTRIBUTED = [d_cluster_mode, d_query_count, d_query_groupby, d_query_vwap,
               d_query_all_pods, d_consistent_results]

HA = [ha_pod_kill, ha_node_drain, ha_pdb_block, ha_rolling_zero_dt, ha_query_during_restart]

PERF = [perf_startup, perf_health_latency, perf_query_latency, perf_failover]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compat-only", action="store_true")
    ap.add_argument("--ha-only", action="store_true")
    ap.add_argument("--perf-only", action="store_true")
    ap.add_argument("--dist-only", action="store_true")
    args = ap.parse_args()

    suite = Suite()
    print("=" * 60)
    print("ZeptoDB Cluster — Full Test Suite")
    print("=" * 60)

    run_all = not any([args.compat_only, args.ha_only, args.perf_only, args.dist_only])

    if run_all or args.compat_only:
        print("\n--- Compatibility (12) ---")
        for t in COMPAT:
            try: t(suite)
            except Exception as e: suite.add(TestResult(t.__name__, False, f"EX: {e}"))

    if run_all or args.dist_only:
        print("\n--- Distributed Queries (6) ---")
        for t in DISTRIBUTED:
            try: t(suite)
            except Exception as e: suite.add(TestResult(t.__name__, False, f"EX: {e}"))

    if run_all or args.ha_only:
        print("\n--- HA (5) ---")
        for t in HA:
            try: t(suite)
            except Exception as e: suite.add(TestResult(t.__name__, False, f"EX: {e}"))

    if run_all or args.perf_only:
        print("\n--- Performance (4) ---")
        for t in PERF:
            try: t(suite)
            except Exception as e: suite.add(TestResult(t.__name__, False, f"EX: {e}"))

    ok = suite.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

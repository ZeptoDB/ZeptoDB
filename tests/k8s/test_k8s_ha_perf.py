#!/usr/bin/env python3.13
"""
ZeptoDB Kubernetes HA & Performance Test Suite

Tests high-availability scenarios that require 3+ nodes and measures
basic K8s-level performance (scheduling latency, rolling update speed,
recovery time, network throughput between pods).

Usage:
    python3.13 tests/k8s/test_k8s_ha_perf.py                # full run
    python3.13 tests/k8s/test_k8s_ha_perf.py --ha-only       # HA tests only
    python3.13 tests/k8s/test_k8s_ha_perf.py --perf-only     # perf benchmarks only
    python3.13 tests/k8s/test_k8s_ha_perf.py --cleanup        # cleanup

Requires: 3-node cluster, helm, kubectl
"""

import argparse
import json
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from statistics import mean, stdev

NAMESPACE = "zeptodb-ha"
RELEASE = "zepto-ha"
CHART_PATH = "deploy/helm/zeptodb"
VALUES_PATH = "tests/k8s/test-values.yaml"
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
class TestSuite:
    results: list[TestResult] = field(default_factory=list)
    perf_results: list[PerfResult] = field(default_factory=list)

    def add(self, r: TestResult):
        self.results.append(r)
        status = "\033[32mPASS\033[0m" if r.passed else "\033[31mFAIL\033[0m"
        print(f"  [{status}] {r.name} ({r.duration_s:.1f}s) {r.message}")

    def add_perf(self, r: PerfResult):
        self.perf_results.append(r)
        print(f"  [PERF] {r.name}: {r.value:.2f} {r.unit}  {r.details}")

    def summary(self) -> bool:
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        print(f"\n{'='*60}")
        print(f"HA Test Results: {passed}/{total} passed, {failed} failed")
        if failed:
            print("\nFailed:")
            for r in self.results:
                if not r.passed:
                    print(f"  - {r.name}: {r.message}")
        if self.perf_results:
            print(f"\n{'─'*60}")
            print("Performance Benchmarks:")
            print(f"  {'Metric':<40} {'Value':>10} {'Unit':<10}")
            print(f"  {'─'*60}")
            for p in self.perf_results:
                print(f"  {p.name:<40} {p.value:>10.2f} {p.unit:<10} {p.details}")
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


def get_pods(label: str = "") -> list[dict]:
    if not label:
        label = f"app.kubernetes.io/instance={RELEASE}"
    data = kubectl_json(f"get pods -l {label}")
    return data.get("items", []) if data else []


def get_ready_pods() -> list[dict]:
    return [
        p for p in get_pods()
        if any(
            c.get("type") == "Ready" and c.get("status") == "True"
            for c in p.get("status", {}).get("conditions", [])
        )
    ]


def wait_ready(count: int, timeout: int = 180) -> tuple[bool, float]:
    """Wait until `count` pods are ready. Returns (success, elapsed_seconds)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if len(get_ready_pods()) >= count:
            return True, time.monotonic() - t0
        time.sleep(3)
    return False, time.monotonic() - t0


# ---------------------------------------------------------------------------
# Setup / Teardown
# ---------------------------------------------------------------------------

def setup():
    print(f"\n--- Setup: namespace={NAMESPACE}, release={RELEASE}, replicas=3 ---")
    run(f"kubectl create namespace {NAMESPACE} --dry-run=client -o yaml | kubectl apply -f -")
    r = run(
        f"helm install {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=3 --wait --timeout 5m",
        timeout=320, check=False,
    )
    if r.returncode != 0:
        print(f"Helm install failed:\n{r.stderr}\n{r.stdout}")
        sys.exit(1)
    # Wait for pods to be fully ready (Karpenter may need extra time)
    ok, elapsed = wait_ready(3, timeout=180)
    if not ok:
        print(f"WARNING: Only {len(get_ready_pods())} pods ready after {elapsed:.0f}s")
    print(f"Helm install succeeded (3 replicas, ready in {elapsed:.0f}s).\n")


def cleanup():
    print(f"\n--- Cleanup ---")
    run(f"helm uninstall {RELEASE} -n {NAMESPACE} --wait", check=False, timeout=120)
    run(f"kubectl delete namespace {NAMESPACE} --wait=false", check=False)
    print("Done.")


# ---------------------------------------------------------------------------
# HA Tests
# ---------------------------------------------------------------------------

def ha_test_3pod_3node_spread(suite: TestSuite):
    """HA01: 3 pods spread across 3 different nodes."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    nodes = {p["spec"]["nodeName"] for p in pods}
    passed = len(pods) == 3 and len(nodes) == 3
    suite.add(TestResult(
        "HA01_3pod_3node_spread", passed,
        f"{len(pods)} pods on {len(nodes)} nodes: {nodes}",
        time.monotonic() - t0,
    ))


def ha_test_node_drain(suite: TestSuite):
    """HA02: Drain a node — pod migrates, service stays available."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        suite.add(TestResult("HA02_node_drain", False, f"need 3 pods, got {len(pods)}", time.monotonic() - t0))
        return

    victim_node = pods[0]["spec"]["nodeName"]

    # Drain the node
    run(f"kubectl drain {victim_node} --ignore-daemonsets --delete-emptydir-data --timeout=60s", check=False, timeout=70)

    # Wait for recovery — 3 pods ready again
    ok, elapsed = wait_ready(3, timeout=120)

    # Check service endpoints
    ep = kubectl_json(f"get endpoints {RELEASE}-zeptodb")
    ep_count = sum(len(s.get("addresses", [])) for s in (ep or {}).get("subsets", []))

    # Uncordon
    run(f"kubectl uncordon {victim_node}", check=False)

    passed = ok and ep_count >= 3
    suite.add(TestResult(
        "HA02_node_drain", passed,
        f"recovery={elapsed:.1f}s, endpoints={ep_count}",
        time.monotonic() - t0,
    ))
    if ok:
        suite.add_perf(PerfResult("drain_recovery_time", elapsed, "sec", f"node={victim_node}"))


def ha_test_pdb_blocks_concurrent_drain(suite: TestSuite):
    """HA03: PDB blocks evicting too many pods (minAvailable=1 with 3 replicas)."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        suite.add(TestResult("HA03_pdb_concurrent_drain", False, f"need 3 pods, got {len(pods)}", time.monotonic() - t0))
        return

    # Evict first two pods via Eviction API — PDB (minAvailable=1) should block
    # the third eviction attempt, but allow the first two since 1 remains.
    # With 3 pods and minAvailable=1, we can evict up to 2. Evicting all 3 should fail.
    evict_results = []
    for p in pods:
        name = p["metadata"]["name"]
        r = kubectl(
            f'create -f - <<EOF\n'
            f'apiVersion: policy/v1\n'
            f'kind: Eviction\n'
            f'metadata:\n'
            f'  name: {name}\n'
            f'  namespace: {NAMESPACE}\n'
            f'EOF'
        )
        evict_results.append((name, r.returncode))

    # At least one eviction should be blocked by PDB
    blocked = sum(1 for _, rc in evict_results if rc != 0)
    passed = blocked >= 1

    # Wait for full recovery
    wait_ready(3, timeout=120)

    suite.add(TestResult(
        "HA03_pdb_concurrent_drain", passed,
        f"evictions: {len(evict_results)}, blocked: {blocked}",
        time.monotonic() - t0,
    ))


def ha_test_pod_kill_recovery(suite: TestSuite):
    """HA04: Kill a pod — auto-recovery, remaining pods serve traffic."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        suite.add(TestResult("HA04_pod_kill", False, f"need 3 pods", time.monotonic() - t0))
        return

    victim = pods[0]["metadata"]["name"]
    src = pods[1]["metadata"]["name"]
    svc = f"{RELEASE}-zeptodb.{NAMESPACE}.svc.cluster.local"

    # Kill pod
    kubectl(f"delete pod {victim} --grace-period=0 --force")

    # Immediately check service is still reachable from another pod
    r = kubectl(f"exec {src} -- wget -q -O /dev/null -T 5 http://{svc}:80/", timeout=15)
    service_available = r.returncode == 0

    # Wait for full recovery
    ok, elapsed = wait_ready(3, timeout=120)

    passed = service_available and ok
    suite.add(TestResult(
        "HA04_pod_kill", passed,
        f"service_during_kill={'OK' if service_available else 'DOWN'}, recovery={elapsed:.1f}s",
        time.monotonic() - t0,
    ))
    if ok:
        suite.add_perf(PerfResult("pod_kill_recovery_time", elapsed, "sec"))


def ha_test_rolling_update_zero_downtime(suite: TestSuite):
    """HA05: Rolling update maintains service availability throughout."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if not pods:
        suite.add(TestResult("HA05_rolling_zero_dt", False, "no pods", time.monotonic() - t0))
        return

    src = pods[0]["metadata"]["name"]
    svc = f"{RELEASE}-zeptodb.{NAMESPACE}.svc.cluster.local"

    # Start rolling update in background thread
    import threading
    def do_upgrade():
        run(
            f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
            f"-f {VALUES_PATH} --set replicaCount=3 --set image.tag=1.27-alpine --wait --timeout 3m",
            check=False, timeout=200,
        )
    t = threading.Thread(target=do_upgrade, daemon=True)
    t.start()

    # Probe service availability during rollout
    failures = 0
    checks = 0
    for _ in range(20):
        time.sleep(3)
        r = kubectl(f"exec {src} -- wget -q -O /dev/null -T 3 http://{svc}:80/", timeout=10)
        checks += 1
        if r.returncode != 0:
            failures += 1

    t.join(timeout=120)
    ok, _ = wait_ready(3, timeout=60)

    passed = ok and failures == 0
    suite.add(TestResult(
        "HA05_rolling_zero_dt", passed,
        f"probes={checks}, failures={failures}",
        time.monotonic() - t0,
    ))


def ha_test_scale_3_to_5_to_3(suite: TestSuite):
    """HA06: Scale 3→5→3 — pods distribute and contract correctly."""
    t0 = time.monotonic()

    # Scale up to 5
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=5 --wait --timeout 5m",
        timeout=320, check=False,
    )
    ok_up, elapsed_up = wait_ready(5, timeout=180)

    # Scale back to 3
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=3 --wait --timeout 3m",
        timeout=200, check=False,
    )
    time.sleep(15)
    pods = get_pods()
    running = [p for p in pods if p["status"].get("phase") == "Running"]

    passed = ok_up and len(running) == 3
    suite.add(TestResult(
        "HA06_scale_3_5_3", passed,
        f"scale_up={elapsed_up:.1f}s, final_running={len(running)}",
        time.monotonic() - t0,
    ))
    if ok_up:
        suite.add_perf(PerfResult("scale_3_to_5_time", elapsed_up, "sec"))


# ---------------------------------------------------------------------------
# Performance Benchmarks
# ---------------------------------------------------------------------------

def perf_pod_startup_latency(suite: TestSuite):
    """PERF01: Measure pod scheduling + startup latency."""
    t0 = time.monotonic()

    # Delete a pod and measure time to ready
    pods = get_ready_pods()
    if not pods:
        suite.add(TestResult("PERF01_startup_latency", False, "no pods", time.monotonic() - t0))
        return

    latencies = []
    for _ in range(3):
        current = get_ready_pods()
        if not current:
            break
        victim = current[0]["metadata"]["name"]
        kubectl(f"delete pod {victim} --grace-period=1")
        time.sleep(2)
        _, elapsed = wait_ready(3, timeout=90)
        latencies.append(elapsed)

    if latencies:
        avg = mean(latencies)
        suite.add_perf(PerfResult("pod_startup_latency_avg", avg, "sec", f"samples={len(latencies)}"))
        if len(latencies) > 1:
            suite.add_perf(PerfResult("pod_startup_latency_stdev", stdev(latencies), "sec"))
    passed = len(latencies) == 3
    suite.add(TestResult("PERF01_startup_latency", passed, f"avg={mean(latencies):.1f}s" if latencies else "no data", time.monotonic() - t0))


def perf_rolling_update_duration(suite: TestSuite):
    """PERF02: Measure rolling update total duration (3 replicas)."""
    t0 = time.monotonic()

    update_start = time.monotonic()
    r = run(
        f"helm upgrade {RELEASE} {CHART_PATH} -n {NAMESPACE} "
        f"-f {VALUES_PATH} --set replicaCount=3 --set image.tag=latest --wait --timeout 5m",
        timeout=320, check=False,
    )
    ok, _ = wait_ready(3, timeout=120)
    update_duration = time.monotonic() - update_start

    passed = r.returncode == 0 and ok
    suite.add(TestResult("PERF02_rolling_update", passed, f"{update_duration:.1f}s", time.monotonic() - t0))
    if passed:
        suite.add_perf(PerfResult("rolling_update_duration_3r", update_duration, "sec", "3 replicas"))

    # Rollback for clean state
    run(f"helm rollback {RELEASE} -n {NAMESPACE} --wait --timeout 3m", check=False, timeout=200)
    wait_ready(3, timeout=120)


def perf_network_latency(suite: TestSuite):
    """PERF03: Measure pod-to-pod network latency (same cluster)."""
    t0 = time.monotonic()
    # Re-fetch pods (previous test may have replaced them via rollback)
    wait_ready(2, timeout=60)
    time.sleep(5)  # Allow containers to fully start
    pods = get_ready_pods()
    if len(pods) < 2:
        suite.add(TestResult("PERF03_network_latency", False, f"need 2+ pods, got {len(pods)}", time.monotonic() - t0))
        return

    src = pods[0]["metadata"]["name"]
    dst_ip = pods[1]["status"]["podIP"]

    # Warm-up request (may fail if container just started)
    kubectl(f"exec {src} -- wget -q -O /dev/null -T 3 http://{dst_ip}:80/", timeout=10)
    time.sleep(1)

    # Measure round-trip via multiple sequential requests timed from client side
    latencies = []
    for _ in range(10):
        req_start = time.monotonic()
        r = kubectl(f"exec {src} -- wget -q -O /dev/null -T 2 http://{dst_ip}:80/", timeout=10)
        req_end = time.monotonic()
        if r.returncode == 0:
            # This includes kubectl exec overhead, so it's an upper bound
            latencies.append((req_end - req_start) * 1000)  # ms

    if latencies:
        avg = mean(latencies)
        suite.add_perf(PerfResult("pod_to_pod_rtt_avg", avg, "ms", f"samples={len(latencies)}, includes kubectl overhead"))
        if len(latencies) > 1:
            suite.add_perf(PerfResult("pod_to_pod_rtt_min", min(latencies), "ms"))
    passed = len(latencies) >= 5
    suite.add(TestResult("PERF03_network_latency", passed, f"avg={mean(latencies):.1f}ms" if latencies else "no data", time.monotonic() - t0))


def perf_network_throughput(suite: TestSuite):
    """PERF04: Measure pod-to-pod HTTP throughput."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 2:
        suite.add(TestResult("PERF04_throughput", False, "need 2+ pods", time.monotonic() - t0))
        return

    src = pods[0]["metadata"]["name"]
    dst_ip = pods[1]["status"]["podIP"]

    # Fire 100 sequential requests, measure total wall time from client side
    req_start = time.monotonic()
    r = kubectl(
        f"exec {src} -- sh -c '"
        f"for i in $(seq 1 100); do wget -q -O /dev/null -T 1 http://{dst_ip}:80/; done"
        f"'",
        timeout=60,
    )
    req_end = time.monotonic()

    if r.returncode == 0:
        total_s = req_end - req_start
        rps = 100 / total_s if total_s > 0 else 0
        suite.add_perf(PerfResult("http_sequential_rps", rps, "req/s", f"100 reqs in {total_s:.2f}s"))
        suite.add(TestResult("PERF04_throughput", True, f"{rps:.0f} req/s", time.monotonic() - t0))
    else:
        suite.add(TestResult("PERF04_throughput", False, r.stderr[:200], time.monotonic() - t0))


def perf_service_failover_time(suite: TestSuite):
    """PERF05: Measure time for service to route around a killed pod."""
    t0 = time.monotonic()
    pods = get_ready_pods()
    if len(pods) < 3:
        suite.add(TestResult("PERF05_failover", False, "need 3 pods", time.monotonic() - t0))
        return

    src = pods[1]["metadata"]["name"]
    victim = pods[0]["metadata"]["name"]
    svc = f"{RELEASE}-zeptodb.{NAMESPACE}.svc.cluster.local"

    # Kill pod
    kubectl(f"delete pod {victim} --grace-period=0 --force")

    # Measure how long until service consistently responds
    fail_start = time.monotonic()
    consecutive_ok = 0
    failover_time = None
    for _ in range(40):
        r = kubectl(f"exec {src} -- wget -q -O /dev/null -T 2 http://{svc}:80/", timeout=10)
        if r.returncode == 0:
            consecutive_ok += 1
            if consecutive_ok >= 3 and failover_time is None:
                failover_time = time.monotonic() - fail_start
        else:
            consecutive_ok = 0
        time.sleep(1)

    wait_ready(3, timeout=120)

    if failover_time is not None:
        suite.add_perf(PerfResult("service_failover_time", failover_time, "sec"))
        suite.add(TestResult("PERF05_failover", True, f"{failover_time:.1f}s", time.monotonic() - t0))
    else:
        suite.add(TestResult("PERF05_failover", False, "service never recovered", time.monotonic() - t0))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

HA_TESTS = [
    ha_test_3pod_3node_spread,
    ha_test_node_drain,
    ha_test_pdb_blocks_concurrent_drain,
    ha_test_pod_kill_recovery,
    ha_test_rolling_update_zero_downtime,
    ha_test_scale_3_to_5_to_3,
]

PERF_TESTS = [
    perf_pod_startup_latency,
    perf_rolling_update_duration,
    perf_network_latency,
    perf_network_throughput,
    perf_service_failover_time,
]


def main():
    parser = argparse.ArgumentParser(description="ZeptoDB K8s HA & Performance Tests")
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("--skip-setup", action="store_true")
    parser.add_argument("--ha-only", action="store_true")
    parser.add_argument("--perf-only", action="store_true")
    args = parser.parse_args()

    if args.cleanup:
        cleanup()
        return

    suite = TestSuite()
    print("=" * 60)
    print("ZeptoDB K8s HA & Performance Test Suite")
    print("=" * 60)

    # Preflight
    r = run("kubectl get nodes --no-headers", check=False, timeout=10)
    node_count = len(r.stdout.strip().split("\n")) if r.returncode == 0 else 0
    print(f"Cluster nodes: {node_count}")
    if node_count < 3:
        print("WARNING: 3+ nodes recommended for HA tests")

    if not args.skip_setup:
        setup()

    tests = []
    if not args.perf_only:
        print("\n--- HA Tests ---")
        tests += HA_TESTS
    if not args.ha_only:
        print("\n--- Performance Benchmarks ---")
        tests += PERF_TESTS

    for t in tests:
        try:
            t(suite)
        except Exception as e:
            suite.add(TestResult(t.__name__, False, f"EXCEPTION: {e}"))

    all_passed = suite.summary()
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()

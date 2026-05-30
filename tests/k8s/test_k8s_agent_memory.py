#!/usr/bin/env python3.13
"""
ZeptoDB Kubernetes Agent Memory E2E Test Suite

Deploys real ZeptoDB pods on the EKS bench node pools and verifies the Agent
Memory HTTP surface end to end on amd64 and arm64:

  - memory put/search/get/context
  - exact and semantic cache lookup
  - tenant isolation and malformed-request handling
  - snapshot/WAL persistence across pod restart
  - delete tombstone replay
  - stats and Prometheus metrics

The test expects current ZeptoDB images to be available in ECR. Defaults match
the bench images built by deploy/scripts/run_arch_comparison_fast.sh.
"""

from __future__ import annotations

import argparse
import contextlib
import http.client
import json
import os
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_REPO = os.environ.get(
    "ZEPTO_EKS_AGENT_IMAGE_REPO",
    "060795905711.dkr.ecr.ap-northeast-2.amazonaws.com/zeptodb",
)
DEFAULT_X86_TAG = os.environ.get("ZEPTO_EKS_AGENT_X86_TAG", "bench-x86")
DEFAULT_ARM64_TAG = os.environ.get("ZEPTO_EKS_AGENT_ARM64_TAG", "bench-arm64")
DEFAULT_STORAGE_CLASS = os.environ.get("ZEPTO_EKS_AGENT_STORAGE_CLASS", "auto")
DEFAULT_VOLUME_MODE = os.environ.get("ZEPTO_EKS_AGENT_VOLUME_MODE", "hostpath")
APP = "zepto-agent-memory"
PORT = 8123


@dataclass
class TestResult:
    name: str
    passed: bool
    message: str = ""
    duration_s: float = 0.0


@dataclass
class TestSuite:
    results: list[TestResult] = field(default_factory=list)

    def add(self, result: TestResult) -> None:
        self.results.append(result)
        status = "\033[32mPASS\033[0m" if result.passed else "\033[31mFAIL\033[0m"
        print(f"  [{status}] {result.name} ({result.duration_s:.1f}s) {result.message}", flush=True)

    def summary(self) -> bool:
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        print(f"\n{'=' * 60}")
        print(f"Agent Memory E2E Results: {passed}/{total} passed, {failed} failed")
        if failed:
            print("\nFailed:")
            for result in self.results:
                if not result.passed:
                    print(f"  - {result.name}: {result.message}")
        print(f"{'=' * 60}")
        return failed == 0


def run(cmd: list[str], *, timeout: int = 120, check: bool = True, input_text: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=check,
        cwd=str(PROJECT_ROOT),
    )


def kubectl(args: list[str], *, namespace: str | None = None, timeout: int = 120, check: bool = False, input_text: str | None = None) -> subprocess.CompletedProcess:
    cmd = ["kubectl"]
    if namespace:
        cmd.extend(["-n", namespace])
    cmd.extend(args)
    return run(cmd, timeout=timeout, check=check, input_text=input_text)


def resolve_storage_class(value: str) -> str:
    if value and value != "auto":
        return value

    r = kubectl(["get", "storageclass", "-o", "json"], timeout=60, check=True)
    data = json.loads(r.stdout)
    items = data.get("items", [])
    if not items:
        raise RuntimeError("no Kubernetes StorageClass is available for the Agent Memory PVC")

    for item in items:
        annotations = item.get("metadata", {}).get("annotations", {})
        if (
            annotations.get("storageclass.kubernetes.io/is-default-class") == "true"
            or annotations.get("storageclass.beta.kubernetes.io/is-default-class") == "true"
        ):
            return item["metadata"]["name"]

    names = {item.get("metadata", {}).get("name") for item in items}
    for preferred in ("gp3", "gp2"):
        if preferred in names:
            return preferred

    return items[0]["metadata"]["name"]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def request(port: int, method: str, path: str, payload: dict[str, Any] | None = None, headers: dict[str, str] | None = None) -> tuple[int, Any]:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, path, body=body, headers=req_headers)
        resp = conn.getresponse()
        raw = resp.read().decode("utf-8", errors="replace")
        try:
            data: Any = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            data = raw
        return resp.status, data
    finally:
        conn.close()


@contextlib.contextmanager
def port_forward(namespace: str):
    local_port = free_port()
    proc: subprocess.Popen[str] | None = None
    try:
        deadline = time.monotonic() + 90
        last_error = ""
        while time.monotonic() < deadline:
            proc = subprocess.Popen(
                ["kubectl", "-n", namespace, "port-forward", f"svc/{APP}", f"{local_port}:{PORT}"],
                cwd=str(PROJECT_ROOT),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            attempt_deadline = min(deadline, time.monotonic() + 15)
            while time.monotonic() < attempt_deadline:
                if proc.poll() is not None:
                    output = proc.stdout.read() if proc.stdout else ""
                    last_error = f"port-forward exited early rc={proc.returncode}: {output}"
                    proc = None
                    break
                try:
                    status, _ = request(local_port, "GET", "/health")
                    if status == 200:
                        yield local_port
                        return
                    last_error = f"status={status}"
                except Exception as exc:  # noqa: BLE001 - startup retry loop
                    last_error = str(exc)
                time.sleep(1)
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                proc = None
            time.sleep(1)
        raise TimeoutError(f"port-forward health check timed out: {last_error}")
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def manifest(
    namespace: str,
    image: str,
    role: str,
    volume_mode: str,
    storage_class: str,
    storage_size: str,
    run_id: str,
) -> str:
    items: list[dict[str, Any]] = [
        {
            "apiVersion": "v1",
            "kind": "Namespace",
            "metadata": {"name": namespace},
        }
    ]

    if volume_mode == "pvc":
        items.append(
            {
                "apiVersion": "v1",
                "kind": "PersistentVolumeClaim",
                "metadata": {"name": "agent-memory-data", "namespace": namespace},
                "spec": {
                    "accessModes": ["ReadWriteOnce"],
                    "storageClassName": storage_class,
                    "resources": {"requests": {"storage": storage_size}},
                },
            }
        )

    volume = (
        {"name": "data", "persistentVolumeClaim": {"claimName": "agent-memory-data"}}
        if volume_mode == "pvc"
        else {
            "name": "data",
            "hostPath": {
                "path": f"/tmp/zeptodb-agent-memory/{namespace}-{run_id}",
                "type": "DirectoryOrCreate",
            },
        }
    )
    items.append(
        {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": APP, "namespace": namespace, "labels": {"app": APP}},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": APP}},
                "strategy": {"type": "Recreate"},
                "template": {
                    "metadata": {"labels": {"app": APP}},
                    "spec": {
                        "nodeSelector": {"zeptodb.com/role": role},
                        "terminationGracePeriodSeconds": 10,
                        "containers": [
                            {
                                "name": "zeptodb",
                                "image": image,
                                "imagePullPolicy": "Always",
                                "args": [
                                    "--port",
                                    str(PORT),
                                    "--no-auth",
                                    "--agent-memory-dir",
                                    "/opt/zeptodb/data/agent_memory",
                                    "--agent-memory-flush-every",
                                    "1",
                                    "--agent-memory-ann",
                                    "auto",
                                    "--agent-memory-ann-min-records",
                                    "2",
                                    "--agent-memory-ann-max-candidates",
                                    "16",
                                    "--agent-memory-max-memories",
                                    "256",
                                    "--agent-memory-max-cache-entries",
                                    "128",
                                ],
                                "ports": [{"name": "http", "containerPort": PORT}],
                                "readinessProbe": {
                                    "httpGet": {"path": "/ready", "port": "http"},
                                    "initialDelaySeconds": 2,
                                    "periodSeconds": 3,
                                    "timeoutSeconds": 2,
                                    "failureThreshold": 20,
                                },
                                "livenessProbe": {
                                    "httpGet": {"path": "/health", "port": "http"},
                                    "initialDelaySeconds": 10,
                                    "periodSeconds": 10,
                                    "timeoutSeconds": 3,
                                    "failureThreshold": 3,
                                },
                                "resources": {
                                    "requests": {"cpu": "500m", "memory": "1Gi"},
                                    "limits": {"cpu": "2", "memory": "4Gi"},
                                },
                                "volumeMounts": [{"name": "data", "mountPath": "/opt/zeptodb/data"}],
                            }
                        ],
                        "volumes": [volume],
                    },
                },
            },
        }
    )
    items.append(
        {
            "apiVersion": "v1",
            "kind": "Service",
            "metadata": {"name": APP, "namespace": namespace},
            "spec": {
                "type": "ClusterIP",
                "selector": {"app": APP},
                "ports": [{"name": "http", "port": PORT, "targetPort": "http"}],
            },
        }
    )
    return json.dumps({"apiVersion": "v1", "kind": "List", "items": items})


def cleanup(namespace: str) -> None:
    kubectl(["delete", "namespace", namespace, "--wait=false"], timeout=90, check=False)


def wait_namespace_deleted(namespace: str, timeout: int = 120) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = kubectl(["get", "namespace", namespace], timeout=30, check=False)
        if r.returncode != 0:
            return
        time.sleep(2)
    raise TimeoutError(f"namespace {namespace} did not terminate within {timeout}s")


def deploy(namespace: str, image: str, role: str, args: argparse.Namespace) -> None:
    yaml = manifest(namespace, image, role, args.volume_mode, args.storage_class, args.storage_size, args.run_id)
    kubectl(["apply", "-f", "-"], input_text=yaml, timeout=120, check=True)
    r = kubectl(["rollout", "status", f"deployment/{APP}", f"--timeout=420s"], namespace=namespace, timeout=450, check=False)
    if r.returncode != 0:
        describe = kubectl(["describe", "pods", "-l", f"app={APP}"], namespace=namespace, timeout=60, check=False)
        logs = kubectl(["logs", "-l", f"app={APP}", "--tail=120"], namespace=namespace, timeout=60, check=False)
        raise RuntimeError(
            f"deployment rollout failed\n{r.stdout}\n{r.stderr}\n"
            f"--- describe ---\n{describe.stdout}\n{describe.stderr}\n"
            f"--- logs ---\n{logs.stdout}\n{logs.stderr}"
        )


def current_pod_node(namespace: str) -> str:
    r = kubectl(["get", "pods", "-l", f"app={APP}", "-o", "json"], namespace=namespace, timeout=60, check=True)
    data = json.loads(r.stdout)
    for item in data.get("items", []):
        node_name = item.get("spec", {}).get("nodeName")
        phase = item.get("status", {}).get("phase")
        if node_name and phase == "Running":
            return node_name
    raise RuntimeError("no running Agent Memory pod has a nodeName")


def restart_pod(namespace: str, volume_mode: str) -> None:
    if volume_mode == "hostpath":
        node_name = current_pod_node(namespace)
        patch = json.dumps({"spec": {"template": {"spec": {"nodeName": node_name}}}})
        kubectl(["patch", "deployment", APP, "--type", "merge", "-p", patch], namespace=namespace, timeout=60, check=True)
    else:
        kubectl(["delete", "pod", "-l", f"app={APP}"], namespace=namespace, timeout=60, check=True)
    r = kubectl(["rollout", "status", f"deployment/{APP}", f"--timeout=420s"], namespace=namespace, timeout=450, check=False)
    if r.returncode != 0:
        raise RuntimeError(f"restart rollout failed: {r.stdout}\n{r.stderr}")


def assert_status(status: int, expected: int, data: Any, context: str) -> None:
    if status != expected:
        raise AssertionError(f"{context}: expected HTTP {expected}, got {status}, body={data}")


def run_agent_flow(local_port: int, arch: str) -> None:
    tenant = f"tenant_{arch}"
    other_tenant = f"tenant_other_{arch}"
    namespace = "agent-e2e"
    pinned_id = f"{arch}_pinned"
    large_id = f"{arch}_large"
    delete_id = f"{arch}_delete"

    status, stats = request(local_port, "GET", "/api/ai/stats")
    assert_status(status, 200, stats, "stats before writes")
    assert stats["memory_count"] >= 0

    status, data = request(local_port, "POST", "/api/ai/memories", None)
    assert_status(status, 400, data, "empty memory body")

    base = {
        "tenant_id": tenant,
        "namespace": namespace,
        "user_id": "u1",
        "session_id": "s1",
        "agent_id": "agent-a",
        "type": "preference",
        "content": "User prefers concise answers.",
        "embedding": [1.0, 0.0],
        "token_count": 5,
        "importance": 2.0,
        "pinned": True,
        "memory_id": pinned_id,
    }
    status, data = request(local_port, "POST", "/api/ai/memories", base)
    assert_status(status, 200, data, "put pinned memory")
    assert data["memory_id"] == pinned_id

    large = dict(base)
    large.update(
        {
            "memory_id": large_id,
            "content": "Very large tool output that should not fit the context budget.",
            "embedding": [0.9, 0.1],
            "token_count": 100,
            "pinned": False,
            "importance": 0.5,
        }
    )
    status, _ = request(local_port, "POST", "/api/ai/memories", large)
    assert_status(status, 200, _, "put large memory")

    deleted = dict(base)
    deleted.update({"memory_id": delete_id, "content": "temporary memory", "embedding": [0.2, 0.8]})
    status, _ = request(local_port, "POST", "/api/ai/memories", deleted)
    assert_status(status, 200, _, "put memory for tombstone")

    wrong_dim = dict(base)
    wrong_dim.update({"memory_id": f"{arch}_wrong_dim", "embedding": [1.0, 0.0, 0.0]})
    status, data = request(local_port, "POST", "/api/ai/memories", wrong_dim)
    assert_status(status, 400, data, "invalid embedding dimension")

    mismatch = dict(base)
    mismatch.update({"memory_id": f"{arch}_mismatch", "tenant_id": "body_tenant"})
    status, data = request(
        local_port,
        "POST",
        "/api/ai/memories",
        mismatch,
        {"X-Zepto-Tenant-Id": "header_tenant"},
    )
    assert_status(status, 400, data, "tenant header mismatch")

    query = {
        "tenant_id": tenant,
        "namespace": namespace,
        "user_id": "u1",
        "query_embedding": [1.0, 0.0],
        "limit": 5,
    }
    status, data = request(local_port, "POST", "/api/ai/memories/search", query)
    assert_status(status, 200, data, "search tenant")
    ids = {m["memory_id"] for m in data["matches"]}
    assert pinned_id in ids, data

    isolated = dict(query)
    isolated["tenant_id"] = other_tenant
    status, data = request(local_port, "POST", "/api/ai/memories/search", isolated)
    assert_status(status, 200, data, "search isolated tenant")
    assert data["rows"] == 0, data

    context = dict(query)
    context.update({"token_budget": 10, "limit": 5})
    status, data = request(local_port, "POST", "/api/ai/context", context)
    assert_status(status, 200, data, "context budget")
    context_ids = {m["memory_id"] for m in data["memories"]}
    assert pinned_id in context_ids, data
    assert large_id not in context_ids, data
    assert data["token_count"] <= 10, data

    exact_cache = {
        "tenant_id": tenant,
        "namespace": namespace,
        "prompt": "  Hello   Agent  ",
        "response": "cached exact response",
        "embedding": [1.0, 0.0],
        "token_count": 4,
    }
    status, data = request(local_port, "POST", "/api/ai/cache/store", exact_cache)
    assert_status(status, 200, data, "store exact cache")

    status, data = request(
        local_port,
        "POST",
        "/api/ai/cache/lookup",
        {"tenant_id": tenant, "namespace": namespace, "prompt": "hello agent"},
    )
    assert_status(status, 200, data, "lookup exact cache")
    assert data["hit"] and data["kind"] == "exact", data

    semantic_cache = {
        "tenant_id": tenant,
        "namespace": namespace,
        "prompt": "summarize robot arm torque",
        "response": "cached semantic response",
        "embedding": [0.0, 1.0],
        "token_count": 6,
    }
    status, data = request(local_port, "POST", "/api/ai/cache/store", semantic_cache)
    assert_status(status, 200, data, "store semantic cache")
    status, data = request(
        local_port,
        "POST",
        "/api/ai/cache/lookup",
        {
            "tenant_id": tenant,
            "namespace": namespace,
            "prompt": "different wording",
            "embedding": [0.0, 0.99],
            "semantic_threshold": 0.5,
        },
    )
    assert_status(status, 200, data, "lookup semantic cache")
    assert data["hit"] and data["kind"] == "semantic", data

    status, data = request(local_port, "DELETE", f"/api/ai/memories/{delete_id}?tenant_id={tenant}&namespace={namespace}")
    assert_status(status, 200, data, "delete memory")
    status, data = request(local_port, "DELETE", f"/api/ai/cache?tenant_id={tenant}&namespace={namespace}&prompt=delete_cache")
    # Non-existent cache delete should surface a clean 404 rather than a crash.
    assert_status(status, 404, data, "delete missing cache")

    delete_cache = {
        "tenant_id": tenant,
        "namespace": namespace,
        "prompt": "delete cache",
        "response": "remove me",
        "embedding": [0.3, 0.7],
    }
    status, _ = request(local_port, "POST", "/api/ai/cache/store", delete_cache)
    assert_status(status, 200, _, "store cache for tombstone")
    status, data = request(local_port, "DELETE", f"/api/ai/cache?tenant_id={tenant}&namespace={namespace}&prompt=delete%20cache")
    assert_status(status, 200, data, "delete cache tombstone")

    status, stats = request(local_port, "GET", "/api/ai/stats")
    assert_status(status, 200, stats, "stats after writes")
    assert stats["memory_count"] >= 2, stats
    assert stats["cache_count"] >= 2, stats
    assert stats["embedding_dim"] == 2, stats
    assert stats["eviction_config"]["max_memories"] == 256, stats
    assert stats["ann"]["mode"] == "auto", stats

    status, metrics = request(local_port, "GET", "/metrics")
    assert_status(status, 200, metrics, "metrics")
    assert "zepto_agent_memory_records" in metrics
    assert "zepto_agent_cache_entries" in metrics


def verify_after_restart(local_port: int, arch: str) -> None:
    tenant = f"tenant_{arch}"
    namespace = "agent-e2e"
    pinned_id = f"{arch}_pinned"
    delete_id = f"{arch}_delete"

    status, data = request(local_port, "GET", f"/api/ai/memories/{pinned_id}?tenant_id={tenant}&namespace={namespace}")
    assert_status(status, 200, data, "get persisted memory")
    assert data["found"] is True and data["memory"]["memory_id"] == pinned_id, data

    status, data = request(local_port, "GET", f"/api/ai/memories/{delete_id}?tenant_id={tenant}&namespace={namespace}")
    assert_status(status, 404, data, "deleted memory tombstone after restart")

    status, data = request(
        local_port,
        "POST",
        "/api/ai/cache/lookup",
        {"tenant_id": tenant, "namespace": namespace, "prompt": "hello agent"},
    )
    assert_status(status, 200, data, "persisted exact cache")
    assert data["hit"] and data["kind"] == "exact", data

    status, data = request(
        local_port,
        "POST",
        "/api/ai/cache/lookup",
        {"tenant_id": tenant, "namespace": namespace, "prompt": "delete cache"},
    )
    assert_status(status, 200, data, "deleted cache tombstone after restart")
    assert data["hit"] is False, data


def run_for_arch(args: argparse.Namespace, arch: str) -> TestResult:
    t0 = time.monotonic()
    role = "bench-x86" if arch == "amd64" else "bench-arm64"
    tag = args.x86_tag if arch == "amd64" else args.arm64_tag
    image = f"{args.image_repo}:{tag}"
    namespace = f"zeptodb-agent-{arch}"

    try:
        if not args.keep:
            cleanup(namespace)
            wait_namespace_deleted(namespace)
        deploy(namespace, image, role, args)
        with port_forward(namespace) as local_port:
            run_agent_flow(local_port, arch)
        restart_pod(namespace, args.volume_mode)
        with port_forward(namespace) as local_port:
            verify_after_restart(local_port, arch)
        return TestResult(f"AgentMemoryE2E_{arch}", True, image, time.monotonic() - t0)
    except Exception as exc:  # noqa: BLE001 - report test failure, then optionally debug
        describe = kubectl(["get", "pods", "-o", "wide"], namespace=namespace, timeout=60, check=False)
        logs = kubectl(["logs", "-l", f"app={APP}", "--tail=160"], namespace=namespace, timeout=60, check=False)
        msg = f"{exc}\n--- pods ---\n{describe.stdout}{describe.stderr}\n--- logs ---\n{logs.stdout}{logs.stderr}"
        return TestResult(f"AgentMemoryE2E_{arch}", False, msg[:4000], time.monotonic() - t0)
    finally:
        if not args.keep:
            cleanup(namespace)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--arch", choices=["all", "amd64", "arm64"], default="all")
    p.add_argument("--image-repo", default=DEFAULT_REPO)
    p.add_argument("--x86-tag", default=DEFAULT_X86_TAG)
    p.add_argument("--arm64-tag", default=DEFAULT_ARM64_TAG)
    p.add_argument("--volume-mode", choices=["hostpath", "pvc"], default=DEFAULT_VOLUME_MODE)
    p.add_argument("--storage-class", default=DEFAULT_STORAGE_CLASS)
    p.add_argument("--storage-size", default=os.environ.get("ZEPTO_EKS_AGENT_STORAGE_SIZE", "1Gi"))
    p.add_argument("--run-id", default=os.environ.get("ZEPTO_EKS_AGENT_RUN_ID", str(int(time.time()))))
    p.add_argument("--keep", action="store_true")
    p.add_argument("--cleanup", action="store_true")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.volume_mode == "pvc":
        args.storage_class = resolve_storage_class(args.storage_class)
        print(f"Using volume mode: pvc storage_class={args.storage_class}", flush=True)
    else:
        print(f"Using volume mode: hostpath run_id={args.run_id}", flush=True)
    arches = ["amd64", "arm64"] if args.arch == "all" else [args.arch]
    suite = TestSuite()
    for arch in arches:
        result = run_for_arch(args, arch)
        suite.add(result)
    ok = suite.summary()
    if args.cleanup:
        for arch in arches:
            cleanup(f"zeptodb-agent-{arch}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

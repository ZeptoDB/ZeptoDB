#!/usr/bin/env python3
"""Offline assertions for the Helm chart's production security defaults."""

from __future__ import annotations

import base64
import hashlib
import subprocess
import unittest
from pathlib import Path

import yaml


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CHART = PROJECT_ROOT / "deploy" / "helm" / "zeptodb"
LEGACY_MANIFEST = PROJECT_ROOT / "deploy" / "k8s" / "deployment.yaml"
DOCKERFILES = (
    PROJECT_ROOT / "deploy" / "docker" / "Dockerfile",
    PROJECT_ROOT / "deploy" / "docker" / "Dockerfile.arm64",
)


def render(*extra_args: str) -> list[dict]:
    result = subprocess.run(
        ["helm", "template", "security-test", str(CHART), *extra_args],
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr.strip()}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def render_error(*extra_args: str) -> str:
    result = subprocess.run(
        ["helm", "template", "security-test", str(CHART), *extra_args],
        cwd=PROJECT_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError("helm template unexpectedly succeeded")
    return result.stderr


def resource(resources: list[dict], kind: str) -> dict:
    return next(item for item in resources if item["kind"] == kind)


def container(workload: dict) -> dict:
    return workload["spec"]["template"]["spec"]["containers"][0]


class HelmSecurityDefaultsTest(unittest.TestCase):
    def test_default_is_single_replica_private_and_authenticated(self) -> None:
        resources = render()
        deployment = resource(resources, "Deployment")
        service = resource(resources, "Service")
        network_policy = resource(resources, "NetworkPolicy")
        pod = deployment["spec"]["template"]
        main = container(deployment)

        self.assertEqual(deployment["spec"]["replicas"], 1)
        self.assertEqual(service["spec"]["type"], "ClusterIP")
        self.assertEqual(network_policy["spec"]["policyTypes"], ["Ingress"])
        self.assertEqual(
            network_policy["spec"]["ingress"][0]["from"],
            [{"podSelector": {}}],
        )
        self.assertEqual(
            deployment["spec"]["strategy"]["rollingUpdate"],
            {"maxSurge": 0, "maxUnavailable": 1},
        )
        self.assertIn("--api-keys-file", main["args"])
        self.assertIn("--no-bootstrap-dev-keys", main["args"])
        self.assertNotIn("--no-auth", main["args"])
        self.assertEqual(main["args"][main["args"].index("--bind") + 1], "0.0.0.0")
        self.assertIn("--allow-plaintext-http", main["args"])
        self.assertIn("--secure-cookie", main["args"])
        self.assertEqual(main["args"][main["args"].index("--ticks") + 1], "0")
        self.assertNotIn("--storage-mode", main["args"])
        self.assertNotIn("--hdb-dir", main["args"])
        self.assertNotIn("--acknowledge-incomplete-durability", main["args"])
        self.assertIn(
            "ZEPTO_CLUSTER_SECRET_FILE",
            {item["name"] for item in main["env"]},
        )
        self.assertIn("checksum/security", pod["metadata"]["annotations"])
        self.assertNotIn("prometheus.io/scrape", pod["metadata"]["annotations"])
        self.assertEqual(pod["spec"]["securityContext"]["runAsUser"], 65532)
        self.assertEqual(pod["spec"]["securityContext"]["fsGroup"], 65532)
        self.assertFalse(pod["spec"]["automountServiceAccountToken"])
        self.assertFalse(main["securityContext"]["allowPrivilegeEscalation"])
        self.assertEqual(main["securityContext"]["capabilities"]["drop"], ["ALL"])
        self.assertNotIn("hugepages-2Mi", main["resources"]["requests"])
        self.assertNotIn("hugepages-2Mi", main["resources"]["limits"])
        self.assertNotIn("hugepage", {item["name"] for item in main["volumeMounts"]})
        self.assertFalse(any(item["kind"] == "HorizontalPodAutoscaler" for item in resources))
        volumes = {
            item["name"]: item for item in pod["spec"]["volumes"]
        }
        self.assertNotIn("data", volumes)
        self.assertFalse(any(item["kind"] == "PersistentVolumeClaim" for item in resources))
        mounted_auth_keys = {
            item["key"] for item in volumes["auth"]["secret"]["items"]
        }
        self.assertEqual(mounted_auth_keys, {"api-keys"})
        self.assertNotIn("admin-api-key", mounted_auth_keys)

        secrets = [item for item in resources if item["kind"] == "Secret"]
        self.assertEqual(len(secrets), 2)
        auth_secret = next(item for item in secrets if item["metadata"]["name"].endswith("-auth"))
        admin_key = auth_secret["stringData"]["admin-api-key"]
        metrics_key = auth_secret["stringData"]["metrics-api-key"]
        key_store = auth_secret["stringData"]["api-keys"]
        key_lines = [line for line in key_store.splitlines() if not line.startswith("#")]
        self.assertTrue(admin_key.startswith("zepto_"))
        self.assertTrue(metrics_key.startswith("zepto_"))
        self.assertEqual(len(key_lines), 2)
        keys_by_id = {line.split("|")[0]: line.split("|") for line in key_lines}
        self.assertEqual(
            keys_by_id["ak_bootstrap"][2],
            hashlib.sha256(admin_key.encode()).hexdigest(),
        )
        self.assertEqual(keys_by_id["ak_bootstrap"][3], "admin")
        self.assertEqual(
            keys_by_id["ak_metrics"][2],
            hashlib.sha256(metrics_key.encode()).hexdigest(),
        )
        self.assertEqual(keys_by_id["ak_metrics"][3], "metrics")

        cluster_secret = next(
            item for item in secrets if item["metadata"]["name"].endswith("-cluster")
        )["stringData"]["cluster-secret"]
        self.assertGreaterEqual(len(base64.b64decode(cluster_secret, validate=True)), 32)

    def test_existing_secrets_are_referenced_without_regeneration(self) -> None:
        resources = render(
            "--set", "auth.existingSecret=operator-auth",
            "--set", "cluster.security.existingSecret=operator-cluster",
            "--set-string", "auth.rolloutChecksum=auth-rotation-42",
            "--set-string", "cluster.security.rolloutChecksum=peer-rotation-7",
        )
        self.assertFalse(any(item["kind"] == "Secret" for item in resources))
        deployment = resource(resources, "Deployment")
        volumes = {
            item["name"]: item for item in deployment["spec"]["template"]["spec"]["volumes"]
        }
        self.assertEqual(volumes["auth"]["secret"]["secretName"], "operator-auth")
        self.assertEqual(
            volumes["cluster-security"]["secret"]["secretName"],
            "operator-cluster",
        )
        annotations = deployment["spec"]["template"]["metadata"]["annotations"]
        self.assertEqual(annotations["checksum/auth-external"], "auth-rotation-42")
        self.assertEqual(annotations["checksum/cluster-external"], "peer-rotation-7")

    def test_published_digest_overrides_the_release_candidate_tag(self) -> None:
        digest = "sha256:" + ("a" * 64)
        resources = render("--set-string", f"image.digest={digest}")
        main = container(resource(resources, "Deployment"))
        self.assertEqual(main["image"], f"zeptodb/zeptodb@{digest}")

    def test_auth_disabled_is_an_explicit_no_auth_override(self) -> None:
        resources = render("--set", "auth.enabled=false")
        deployment = resource(resources, "Deployment")
        main = container(deployment)
        self.assertIn("--no-auth", main["args"])
        self.assertNotIn("--api-keys-file", main["args"])
        self.assertNotIn(
            "auth",
            {item["name"] for item in deployment["spec"]["template"]["spec"]["volumes"]},
        )

    def test_cluster_mode_mounts_persistent_data_and_peer_secret(self) -> None:
        resources = render(
            "--set", "cluster.enabled=true",
            "--set", "replicaCount=3",
            "--set", "persistence.enabled=true",
            "--set", "persistence.acknowledgeIncompleteDurability=true",
        )
        stateful_set = resource(resources, "StatefulSet")
        network_policy = resource(resources, "NetworkPolicy")
        self.assertEqual(stateful_set["spec"]["replicas"], 3)
        main = container(stateful_set)
        self.assertIn("data", {item["name"] for item in main["volumeMounts"]})
        self.assertIn(
            "ZEPTO_CLUSTER_SECRET_FILE",
            {item["name"] for item in main["env"]},
        )
        script = stateful_set["spec"]["template"]["spec"]["initContainers"][0]["args"][0]
        self.assertIn("--api-keys-file", script)
        self.assertIn("--bind 0.0.0.0", script)
        self.assertIn("--allow-plaintext-http", script)
        self.assertIn("--secure-cookie", script)
        self.assertIn("--no-bootstrap-dev-keys", script)
        self.assertNotIn("--no-auth", script)
        self.assertIn("--ticks 0", script)
        self.assertIn("--storage-mode tiered", script)
        self.assertIn("--hdb-dir /opt/zeptodb/data", script)
        self.assertIn("--acknowledge-incomplete-durability", script)
        self.assertEqual(
            stateful_set["spec"]["template"]["spec"]["securityContext"]["fsGroup"],
            65532,
        )
        rpc_rule = next(
            rule
            for rule in network_policy["spec"]["ingress"]
            if any(port["port"] == 8223 for port in rule["ports"])
        )
        self.assertEqual(
            rpc_rule["from"][0]["podSelector"]["matchLabels"],
            {"app.kubernetes.io/name": "zeptodb", "app.kubernetes.io/instance": "security-test"},
        )

    def test_ingest_requires_explicit_benchmark_no_auth_and_routes(self) -> None:
        default_error = render_error("--set", "ingest.enabled=true")
        self.assertIn("zepto_ingest_node cannot load the API-key store", default_error)

        missing_routes_error = render_error(
            "--set", "ingest.enabled=true",
            "--set", "ingest.noAuth=true",
        )
        self.assertIn("ingest.extraArgs must include explicit --add-node", missing_routes_error)

        resources = render(
            "--set", "ingest.enabled=true",
            "--set", "ingest.noAuth=true",
            "--set-json", 'ingest.extraArgs=["--add-node","0:storage:8123"]',
        )
        deployments = [item for item in resources if item["kind"] == "Deployment"]
        ingest = next(
            item
            for item in deployments
            if item["metadata"]["name"].endswith("-ingest")
        )
        main = container(ingest)
        self.assertIn("--no-auth", main["args"])
        self.assertIn("--add-node", main["args"])
        self.assertIn(
            "ZEPTO_CLUSTER_SECRET_FILE",
            {item["name"] for item in main["env"]},
        )

    def test_unsafe_standalone_scale_is_rejected(self) -> None:
        replica_error = render_error("--set", "replicaCount=2")
        self.assertIn("standalone mode requires replicaCount=1", replica_error)
        hpa_error = render_error("--set", "autoscaling.enabled=true")
        self.assertIn("StatefulSet peer discovery", hpa_error)
        cluster_hpa_error = render_error(
            "--set", "cluster.enabled=true",
            "--set", "replicaCount=3",
            "--set", "autoscaling.enabled=true",
        )
        self.assertIn("StatefulSet peer discovery", cluster_hpa_error)

    def test_authenticated_metrics_use_a_dedicated_servicemonitor_key(self) -> None:
        resources = render("--set", "serviceMonitor.enabled=true")
        monitor = resource(resources, "ServiceMonitor")
        authorization = monitor["spec"]["endpoints"][0]["authorization"]
        self.assertEqual(authorization["type"], "Bearer")
        self.assertEqual(authorization["credentials"]["key"], "metrics-api-key")
        self.assertTrue(authorization["credentials"]["name"].endswith("-auth"))

        annotation_error = render_error("--set", "prometheus.scrape=true")
        self.assertIn("prometheus.scrape cannot authenticate", annotation_error)
        monitor_error = render_error(
            "--set", "serviceMonitor.enabled=true",
            "--set", "serviceMonitor.authorization.enabled=false",
        )
        self.assertIn("serviceMonitor.authorization.enabled must be true", monitor_error)

    def test_persistence_requires_incomplete_durability_acknowledgement(self) -> None:
        error = render_error("--set", "persistence.enabled=true")
        self.assertIn("does not yet guarantee SQL-visible restart durability", error)

        resources = render(
            "--set", "persistence.enabled=true",
            "--set", "persistence.acknowledgeIncompleteDurability=true",
        )
        deployment = resource(resources, "Deployment")
        main = container(deployment)
        self.assertEqual(
            main["args"][main["args"].index("--storage-mode") + 1],
            "tiered",
        )
        self.assertEqual(
            main["args"][main["args"].index("--hdb-dir") + 1],
            "/opt/zeptodb/data",
        )
        self.assertIn("--acknowledge-incomplete-durability", main["args"])
        self.assertIn(
            "data",
            {item["name"] for item in deployment["spec"]["template"]["spec"]["volumes"]},
        )
        self.assertTrue(any(item["kind"] == "PersistentVolumeClaim" for item in resources))

        disabled_main = container(resource(render(), "Deployment"))
        self.assertNotIn("--storage-mode", disabled_main["args"])
        self.assertNotIn("--hdb-dir", disabled_main["args"])
        self.assertNotIn(
            "--acknowledge-incomplete-durability", disabled_main["args"]
        )

    def test_hugepages_require_an_explicit_reserved_resource_profile(self) -> None:
        missing_error = render_error(
            "--set", "performanceTuning.hugepages.enabled=true",
        )
        self.assertIn("requires matching resources.requests.hugepages-2Mi", missing_error)

        resources = render(
            "--set", "performanceTuning.hugepages.enabled=true",
            "--set-string", "resources.requests.hugepages-2Mi=1Gi",
            "--set-string", "resources.limits.hugepages-2Mi=1Gi",
        )
        main = container(resource(resources, "Deployment"))
        self.assertEqual(main["resources"]["requests"]["hugepages-2Mi"], "1Gi")
        self.assertIn("hugepage", {item["name"] for item in main["volumeMounts"]})

    def test_insecure_cluster_requires_explicit_runtime_override(self) -> None:
        resources = render(
            "--set", "cluster.enabled=true",
            "--set", "replicaCount=3",
            "--set", "cluster.security.enabled=false",
        )
        stateful_set = resource(resources, "StatefulSet")
        script = stateful_set["spec"]["template"]["spec"]["initContainers"][0]["args"][0]
        self.assertIn("--allow-insecure-cluster", script)

    def test_legacy_manifest_keeps_the_same_production_boundaries(self) -> None:
        resources = [
            doc for doc in yaml.safe_load_all(LEGACY_MANIFEST.read_text()) if doc
        ]
        deployment = resource(resources, "Deployment")
        service = next(
            item
            for item in resources
            if item["kind"] == "Service" and item["metadata"]["name"] == "zeptodb-service"
        )
        pod = deployment["spec"]["template"]
        main = container(deployment)

        self.assertEqual(deployment["spec"]["replicas"], 1)
        self.assertEqual(service["spec"]["type"], "ClusterIP")
        self.assertEqual(resource(resources, "NetworkPolicy")["spec"]["policyTypes"], ["Ingress"])
        self.assertFalse(any(item["kind"] == "HorizontalPodAutoscaler" for item in resources))
        self.assertNotIn("prometheus.io/scrape", pod["metadata"].get("annotations", {}))
        self.assertEqual(pod["spec"]["securityContext"]["fsGroup"], 65532)
        self.assertFalse(pod["spec"]["automountServiceAccountToken"])
        self.assertFalse(main["securityContext"]["allowPrivilegeEscalation"])
        self.assertEqual(main["args"][main["args"].index("--bind") + 1], "0.0.0.0")
        self.assertIn("--allow-plaintext-http", main["args"])
        self.assertIn("--secure-cookie", main["args"])
        self.assertEqual(main["args"][main["args"].index("--ticks") + 1], "0")
        self.assertIn("--storage-mode", main["args"])
        self.assertIn("--hdb-dir", main["args"])
        self.assertIn("--acknowledge-incomplete-durability", main["args"])
        self.assertIn("--api-keys-file", main["args"])
        self.assertNotIn("--no-auth", main["args"])

    def test_production_docker_defaults_are_fail_closed_and_idle(self) -> None:
        for dockerfile in DOCKERFILES:
            with self.subTest(dockerfile=dockerfile.name):
                cmd = [
                    line for line in dockerfile.read_text().splitlines()
                    if line.startswith("CMD ")
                ][-1]
                self.assertIn('"--bind", "0.0.0.0"', cmd)
                self.assertIn('"--allow-plaintext-http"', cmd)
                self.assertIn('"--secure-cookie"', cmd)
                self.assertIn('"--ticks", "0"', cmd)
                self.assertIn('"--api-keys-file"', cmd)
                self.assertIn('"--no-bootstrap-dev-keys"', cmd)
                self.assertNotIn('"--no-auth"', cmd)


if __name__ == "__main__":
    unittest.main()

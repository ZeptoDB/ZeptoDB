#!/usr/bin/env python3.13
"""Run the Physical AI Agent Memory replay against real EKS ZeptoDB images."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path
from types import SimpleNamespace


PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
TOOLS_DIR = PROJECT_ROOT / "docs" / "research" / "tools"
sys.path.insert(0, str(PROJECT_ROOT))
sys.path.insert(0, str(TOOLS_DIR))

from tests.k8s import test_k8s_agent_memory as agent_e2e  # noqa: E402

import physical_ai_agent_memory_replay as replay  # noqa: E402


def run_arch(args: argparse.Namespace, arch: str) -> dict:
    namespace = f"zeptodb-vla-memory-{arch}-{args.namespace_suffix}"
    role = "bench-x86" if arch == "amd64" else "bench-arm64"
    tag = args.x86_tag if arch == "amd64" else args.arm64_tag
    image = f"{args.image_repo}:{tag}"
    deploy_args = SimpleNamespace(
        volume_mode="hostpath",
        storage_class="auto",
        storage_size="1Gi",
        run_id=args.run_id,
    )

    agent_e2e.cleanup(namespace)
    agent_e2e.wait_namespace_deleted(namespace)
    try:
        agent_e2e.deploy(namespace, image, role, deploy_args)
        with agent_e2e.port_forward(namespace) as local_port:
            client = replay.AgentMemoryClient(
                f"http://127.0.0.1:{local_port}",
                timeout=args.timeout,
            )
            return replay.run_experiment(
                client,
                replay.load_fixture(args.fixture),
                arch=arch,
                run_id=args.run_id,
                top_k=args.top_k,
                repeats=args.repeats,
            )
    finally:
        agent_e2e.cleanup(namespace)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=["all", "amd64", "arm64"], default="all")
    parser.add_argument("--image-repo", default=agent_e2e.DEFAULT_REPO)
    parser.add_argument("--x86-tag", default=agent_e2e.DEFAULT_X86_TAG)
    parser.add_argument("--arm64-tag", default=agent_e2e.DEFAULT_ARM64_TAG)
    parser.add_argument("--fixture", type=Path, default=replay.DEFAULT_FIXTURE)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/research/results/physical_ai_agent_memory_eks_replay_024.md"),
    )
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--run-id", default=str(int(time.time())))
    parser.add_argument("--namespace-suffix")
    parser.add_argument("--top-k", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=10.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.namespace_suffix is None:
        args.namespace_suffix = re.sub(r"[^a-z0-9-]", "", args.run_id.lower())[-12:]
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,10}[a-z0-9])?", args.namespace_suffix):
        raise SystemExit("--namespace-suffix must be 1-12 DNS-label characters")
    for output in (args.output, args.json_output):
        if output is not None and output.exists():
            raise SystemExit(f"Refusing to overwrite immutable result: {output}")

    arches = ["amd64", "arm64"] if args.arch == "all" else [args.arch]
    results = []
    for arch in arches:
        print(f"Running Physical AI Agent Memory replay on {arch}", flush=True)
        result = run_arch(args, arch)
        results.append(result)
        search = result["latency"]["search_warm"]
        gated = result["quality"]["context_gated_prior"]
        print(
            f"  {arch}: search_p95={search['p95_ms']:.3f}ms "
            f"recovery_top1={gated['recovery_top1_hit_rate']:.2f}",
            flush=True,
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as output_file:
        output_file.write(replay.render_report(results, args.fixture))
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        with args.json_output.open("x") as output_file:
            output_file.write(json.dumps(results, indent=2, sort_keys=True) + "\n")

    failed = any(
        result["quality"]["context_gated_prior"]["recovery_top1_hit_rate"] < 1.0
        or result["quality"]["context_gated_prior"]["hazardous_top_action_rate"] > 0.0
        or result["latency"]["search_warm"]["p95_ms"] >= 100.0
        for result in results
    )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

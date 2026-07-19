#!/usr/bin/env python3
"""Replay Physical AI action-outcome memory through Agent Memory HTTP APIs."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import physical_ai_action_outcome_baseline as baseline


DEFAULT_FIXTURE = Path("docs/research/fixtures/physical_ai_action_outcome_episodes.json")
DEFAULT_QUERY_IDS = baseline.DEFAULT_QUERY_IDS
EMBEDDING_DIM = 64
OBSERVATION_FIELDS = (
    "incident_type",
    "domain",
    "site",
    "symptoms",
    "temporal_motif",
    "topology_context",
    "change_context",
)


class AgentMemoryClient:
    def __init__(self, base_url: str, timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[dict[str, Any], float]:
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=body,
            method=method,
            headers={"Content-Type": "application/json"},
        )
        start = time.perf_counter_ns()
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"{method} {path} returned HTTP {exc.code}: {raw}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"{method} {path} failed: {exc.reason}") from exc
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        try:
            data = json.loads(raw) if raw else {}
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"{method} {path} returned invalid JSON") from exc
        if not isinstance(data, dict):
            raise RuntimeError(f"{method} {path} returned a non-object JSON response")
        return data, elapsed_ms


def _feature_tokens(value: Any, prefix: str = "") -> list[str]:
    if isinstance(value, dict):
        tokens: list[str] = []
        for key in sorted(value):
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            tokens.append(child_prefix)
            tokens.extend(_feature_tokens(value[key], child_prefix))
        return tokens
    if isinstance(value, list):
        tokens = []
        for item in value:
            tokens.extend(_feature_tokens(item, prefix))
        return tokens
    if isinstance(value, bool):
        return [f"{prefix}={str(value).lower()}"]
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        number = float(value)
        if not math.isfinite(number):
            return [f"{prefix}=non_finite"]
        if number == 0:
            bucket = "zero"
        else:
            sign = "neg" if number < 0 else "pos"
            bucket = f"{sign}_e{int(math.floor(math.log10(abs(number))))}"
        return [f"{prefix}={bucket}"]
    if value is None:
        return [f"{prefix}=null"]
    return [f"{prefix}={str(value).lower()}"]


def observation_embedding(episode: dict[str, Any], dimensions: int = EMBEDDING_DIM) -> list[float]:
    if dimensions <= 0:
        raise ValueError("embedding dimensions must be positive")
    observation = {field: episode.get(field) for field in OBSERVATION_FIELDS}
    vector = [0.0] * dimensions
    for token in _feature_tokens(observation):
        digest = hashlib.blake2b(token.encode("utf-8"), digest_size=16).digest()
        index = int.from_bytes(digest[:8], "little") % dimensions
        vector[index] += 1.0 if digest[8] & 1 else -1.0
    norm = math.sqrt(sum(value * value for value in vector))
    if norm == 0:
        raise ValueError("episode produced an empty observation embedding")
    return [value / norm for value in vector]


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    if not 0.0 <= quantile <= 1.0:
        raise ValueError("quantile must be between 0 and 1")
    ordered = sorted(values)
    index = max(0, math.ceil(quantile * len(ordered)) - 1)
    return ordered[index]


def latency_summary(values: list[float]) -> dict[str, float]:
    return {
        "count": float(len(values)),
        "mean_ms": statistics.fmean(values) if values else 0.0,
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
        "max_ms": max(values, default=0.0),
    }


def load_fixture(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text())
    episodes = data.get("episodes")
    if not isinstance(episodes, list) or not episodes:
        raise ValueError("fixture must contain a non-empty episodes array")
    return episodes


def _memory_payload(
    episode: dict[str, Any],
    *,
    tenant_id: str,
    namespace: str,
    run_id: str,
) -> dict[str, Any]:
    content = json.dumps(
        {
            "incident_type": episode["incident_type"],
            "action": episode["action"]["action_class"],
            "outcome": baseline.outcome_label(episode),
            "reflection": episode.get("reflection", ""),
        },
        sort_keys=True,
    )
    return {
        "memory_id": f"{run_id}_{episode['episode_id']}",
        "tenant_id": tenant_id,
        "namespace": namespace,
        "agent_id": "physical-ai-vla-replay",
        "type": "physical_ai_action_outcome",
        "content": content,
        "metadata_json": json.dumps({"episode": episode}, sort_keys=True),
        "embedding": observation_embedding(episode),
        "token_count": max(1, len(content) // 4),
        "importance": 0.0,
        "created_at_ns": int(episode.get("action_ts_ns", 0)),
    }


def _decision_metrics(query: dict[str, Any], action: str) -> dict[str, Any]:
    safe = set(query.get("expected_safe_actions", []))
    unsafe = set(query.get("unsafe_repeat_actions", []))
    return {
        "top_action": action,
        "recovery_top1_hit": action in safe,
        "risky_repeat_avoidance": action not in unsafe,
        "hazardous_top_action": action in unsafe,
    }


def _aggregate_quality(per_query: list[dict[str, Any]], variant: str) -> dict[str, float]:
    rows = [query["decisions"][variant] for query in per_query]
    count = len(rows)
    if count == 0:
        return {
            "recovery_top1_hit_rate": 0.0,
            "risky_repeat_avoidance_rate": 0.0,
            "hazardous_top_action_rate": 0.0,
        }
    return {
        "recovery_top1_hit_rate": sum(row["recovery_top1_hit"] for row in rows) / count,
        "risky_repeat_avoidance_rate": sum(row["risky_repeat_avoidance"] for row in rows) / count,
        "hazardous_top_action_rate": sum(row["hazardous_top_action"] for row in rows) / count,
    }


def run_experiment(
    client: AgentMemoryClient,
    episodes: list[dict[str, Any]],
    *,
    arch: str,
    run_id: str,
    query_ids: list[str] | None = None,
    top_k: int = 8,
    repeats: int = 20,
) -> dict[str, Any]:
    if top_k <= 0:
        raise ValueError("top_k must be positive")
    if repeats <= 0:
        raise ValueError("repeats must be positive")
    if not episodes:
        raise ValueError("episodes must not be empty")

    selected_query_ids = query_ids or DEFAULT_QUERY_IDS
    by_id = {episode["episode_id"]: episode for episode in episodes}
    missing = [query_id for query_id in selected_query_ids if query_id not in by_id]
    if missing:
        raise ValueError(f"unknown query ids: {', '.join(missing)}")

    tenant_id = f"physical-ai-024-{run_id}-{arch}"
    namespace = "vla-memory-replay"
    memories = [
        episode
        for episode in episodes
        if episode["episode_id"] not in selected_query_ids
    ]

    insert_latencies: list[float] = []
    for episode in memories:
        response, elapsed_ms = client.request(
            "POST",
            "/api/ai/memories",
            _memory_payload(
                episode,
                tenant_id=tenant_id,
                namespace=namespace,
                run_id=run_id,
            ),
        )
        if not response.get("ok"):
            raise RuntimeError(f"memory insert failed for {episode['episode_id']}: {response}")
        insert_latencies.append(elapsed_ms)

    per_query: list[dict[str, Any]] = []
    search_latencies: list[float] = []
    prior_latencies: list[float] = []
    for query_id in selected_query_ids:
        query = by_id[query_id]
        request = {
            "tenant_id": tenant_id,
            "namespace": namespace,
            "agent_id": "physical-ai-vla-replay",
            "type": "physical_ai_action_outcome",
            "query_embedding": observation_embedding(query),
            "limit": top_k,
        }
        response, cold_ms = client.request("POST", "/api/ai/memories/search", request)
        matches = response.get("matches", [])
        if not matches:
            raise RuntimeError(f"Agent Memory returned no matches for {query_id}")

        for _ in range(repeats):
            _, elapsed_ms = client.request("POST", "/api/ai/memories/search", request)
            search_latencies.append(elapsed_ms)

        rows: list[tuple[float, dict[str, Any]]] = []
        retrieval = []
        for rank, match in enumerate(matches, start=1):
            metadata = json.loads(match["metadata_json"])
            candidate = metadata["episode"]
            similarity = float(match["similarity"])
            rows.append((similarity, candidate))
            retrieval.append(
                {
                    "rank": rank,
                    "episode_id": candidate["episode_id"],
                    "action": candidate["action"]["action_class"],
                    "outcome": baseline.outcome_label(candidate),
                    "similarity": similarity,
                }
            )

        prior_start = time.perf_counter_ns()
        outcome_actions, _ = baseline.action_rank(
            rows,
            use_outcome=True,
            limit=top_k,
        )
        gated_actions, suppressions = baseline.action_rank(
            rows,
            use_outcome=True,
            query=query,
            gated=True,
            limit=top_k,
        )
        prior_ms = (time.perf_counter_ns() - prior_start) / 1_000_000
        prior_latencies.append(prior_ms)

        raw_action = rows[0][1]["action"]["action_class"]
        outcome_action = outcome_actions[0][0] if outcome_actions else "none"
        gated_action = gated_actions[0][0] if gated_actions else "none"
        per_query.append(
            {
                "query_id": query_id,
                "cold_search_ms": cold_ms,
                "retrieval": retrieval,
                "suppression_count": len(suppressions),
                "decisions": {
                    "no_memory": _decision_metrics(query, query["action"]["action_class"]),
                    "raw_retrieval": _decision_metrics(query, raw_action),
                    "outcome_prior": _decision_metrics(query, outcome_action),
                    "context_gated_prior": _decision_metrics(query, gated_action),
                },
            }
        )

    variants = ("no_memory", "raw_retrieval", "outcome_prior", "context_gated_prior")
    return {
        "arch": arch,
        "run_id": run_id,
        "memory_records": len(memories),
        "queries": len(selected_query_ids),
        "top_k": top_k,
        "repeats": repeats,
        "embedding": {
            "kind": "deterministic_observation_feature_hash",
            "dimensions": EMBEDDING_DIM,
            "server_provider_calls": 0,
        },
        "latency": {
            "insert": latency_summary(insert_latencies),
            "search_warm": latency_summary(search_latencies),
            "search_cold": latency_summary([row["cold_search_ms"] for row in per_query]),
            "compact_prior_compute": latency_summary(prior_latencies),
        },
        "quality": {variant: _aggregate_quality(per_query, variant) for variant in variants},
        "per_query": per_query,
    }


def render_report(results: list[dict[str, Any]], fixture: Path) -> str:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Physical AI Agent Memory EKS Replay 024 Results",
        "",
        f"Generated at: {generated_at}",
        "Classification: Research-only",
        f"Fixture: `{fixture}`",
        "",
        "## Scope",
        "",
        "This run validates the retrieval and compact-prior layer on real ZeptoDB",
        "EKS images. It uses deterministic observation feature hashing as a",
        "vision-language embedding proxy and does not invoke a VLA model or GPU.",
        "Reported search latency includes the kubectl port-forward client round trip.",
        "",
        "## Runtime Summary",
        "",
        "| Arch | Memories | Queries | Insert p50 ms | Insert p95 ms | Warm search p50 ms | Warm search p95 ms | Prior p95 ms |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        insert = result["latency"]["insert"]
        search = result["latency"]["search_warm"]
        prior = result["latency"]["compact_prior_compute"]
        lines.append(
            f"| {result['arch']} | {result['memory_records']} | {result['queries']} | "
            f"{insert['p50_ms']:.3f} | {insert['p95_ms']:.3f} | "
            f"{search['p50_ms']:.3f} | {search['p95_ms']:.3f} | "
            f"{prior['p95_ms']:.3f} |"
        )

    lines += [
        "",
        "## Decision Quality",
        "",
        "| Arch | Variant | Recovery Top-1 | Risky Repeat Avoidance | Hazardous Top Action |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for result in results:
        for variant, quality in result["quality"].items():
            lines.append(
                f"| {result['arch']} | {variant} | "
                f"{quality['recovery_top1_hit_rate']:.2f} | "
                f"{quality['risky_repeat_avoidance_rate']:.2f} | "
                f"{quality['hazardous_top_action_rate']:.2f} |"
            )

    lines += [
        "",
        "## Per-Query Decisions",
        "",
        "| Arch | Query | No Memory | Raw Retrieval | Outcome Prior | Context-Gated Prior | Suppressions |",
        "| --- | --- | --- | --- | --- | --- | ---: |",
    ]
    for result in results:
        for query in result["per_query"]:
            decisions = query["decisions"]
            lines.append(
                f"| {result['arch']} | {query['query_id']} | "
                f"{decisions['no_memory']['top_action']} | "
                f"{decisions['raw_retrieval']['top_action']} | "
                f"{decisions['outcome_prior']['top_action']} | "
                f"{decisions['context_gated_prior']['top_action']} | "
                f"{query['suppression_count']} |"
            )

    decisions_by_arch = [
        [
            query["decisions"]["context_gated_prior"]["top_action"]
            for query in result["per_query"]
        ]
        for result in results
    ]
    cross_arch_match = len(decisions_by_arch) < 2 or all(
        decisions == decisions_by_arch[0] for decisions in decisions_by_arch[1:]
    )
    acceptance = []
    for result in results:
        gated = result["quality"]["context_gated_prior"]
        acceptance.extend(
            [
                (f"{result['arch']} returned all five queries", result["queries"] == 5),
                (
                    f"{result['arch']} context-gated recovery Top-1 is 1.00",
                    gated["recovery_top1_hit_rate"] == 1.0,
                ),
                (
                    f"{result['arch']} hazardous top-action rate is 0.00",
                    gated["hazardous_top_action_rate"] == 0.0,
                ),
                (
                    f"{result['arch']} warm search p95 is below 100 ms",
                    result["latency"]["search_warm"]["p95_ms"] < 100.0,
                ),
            ]
        )
    acceptance.append(("Context-gated decisions match across architectures", cross_arch_match))

    lines += [
        "",
        "## Acceptance",
        "",
        "| Criterion | Status |",
        "| --- | --- |",
    ]
    for criterion, passed in acceptance:
        lines.append(f"| {criterion} | {'pass' if passed else 'fail'} |")

    overall = all(passed for _, passed in acceptance)
    lines += [
        "",
        "## Result",
        "",
        f"Overall status: {'pass' if overall else 'fail'}.",
        "",
        "## Interpretation",
        "",
        "This is infrastructure and retrieval-policy evidence, not an end-to-end VLA",
        "claim. The next experiment must replace the proxy embedding with a real",
        "vision-language encoder and measure model inference latency, GPU time,",
        "input tokens, and task success on held-out simulator episodes.",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--arch", default="unknown")
    parser.add_argument("--run-id", default=str(int(time.time())))
    parser.add_argument("--top-k", type=int, default=8)
    parser.add_argument("--repeats", type=int, default=20)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    episodes = load_fixture(args.fixture)
    result = run_experiment(
        AgentMemoryClient(args.url, timeout=args.timeout),
        episodes,
        arch=args.arch,
        run_id=args.run_id,
        top_k=args.top_k,
        repeats=args.repeats,
    )
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(render_report([result], args.fixture))
    if not args.output and not args.json_output:
        print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

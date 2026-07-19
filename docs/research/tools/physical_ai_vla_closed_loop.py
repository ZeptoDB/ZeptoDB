#!/usr/bin/env python3
"""Run research-only Experiment 027 in the closed-loop LIBERO simulator."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import physical_ai_vla_early_exit as exp26  # noqa: E402


EXPERIMENT = 27
SUITE = "libero_10"
VLA_MODEL = exp26.VLA_MODEL
VLA_MODEL_SHA = exp26.VLA_MODEL_SHA
SIGLIP_MODEL = exp26.SIGLIP_MODEL
DEFAULT_THRESHOLD = 0.8908413128714854


def percentile(values: list[float], quantile: float) -> float:
    return exp26.percentile(values, quantile)


def latency_summary(values: list[float]) -> dict[str, float | int]:
    return exp26.latency_summary(values)


def route_action(
    *,
    confidence: float,
    threshold: float,
    task_match: bool,
    high_confidence_streak: int,
    consecutive_skips: int,
    required_streak: int,
    max_consecutive_skips: int,
) -> tuple[bool, int]:
    if required_streak <= 0 or max_consecutive_skips <= 0:
        raise ValueError("router streak limits must be positive")
    next_streak = high_confidence_streak + 1 if task_match and confidence >= threshold else 0
    should_skip = (
        next_streak >= required_streak and consecutive_skips < max_consecutive_skips
    )
    return should_skip, next_streak


def extract_success(info: dict[str, Any]) -> bool:
    final_info = info.get("final_info")
    if isinstance(final_info, dict) and "is_success" in final_info:
        value = final_info["is_success"]
    else:
        value = info.get("is_success", False)
    if hasattr(value, "reshape"):
        flattened = value.reshape(-1)
        return bool(flattened[0]) if len(flattened) else False
    if isinstance(value, (list, tuple)):
        return bool(value[0]) if value else False
    return bool(value)


def paired_quality(
    task_results: list[dict[str, Any]],
) -> dict[str, float | int]:
    if not task_results:
        raise ValueError("task results must not be empty")
    baseline_successes = sum(bool(row["baseline_success"]) for row in task_results)
    routed_successes = sum(bool(row["routed_success"]) for row in task_results)
    regressions = sum(
        bool(row["baseline_success"]) and not bool(row["routed_success"])
        for row in task_results
    )
    improvements = sum(
        not bool(row["baseline_success"]) and bool(row["routed_success"])
        for row in task_results
    )
    count = len(task_results)
    return {
        "tasks": count,
        "baseline_successes": baseline_successes,
        "routed_successes": routed_successes,
        "baseline_success_rate": baseline_successes / count,
        "routed_success_rate": routed_successes / count,
        "success_rate_delta": (routed_successes - baseline_successes) / count,
        "paired_regressions": regressions,
        "paired_improvements": improvements,
    }


def resolve_memory_task_index(
    task_description: str, text_to_task: dict[str, int]
) -> int:
    if task_description not in text_to_task:
        raise RuntimeError(
            f"LIBERO task text is absent from memory bank: {task_description}"
        )
    return int(text_to_task[task_description])


def seed_policy_rng(seed: int, torch_module: Any | None = None) -> None:
    if torch_module is None:
        import torch as torch_module

    torch_module.manual_seed(seed)
    torch_module.cuda.manual_seed_all(seed)


class AgentMemoryClient:
    def __init__(self, base_url: str, timeout: float = 15.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(
        self, method: str, path: str, payload: dict[str, Any]
    ) -> tuple[dict[str, Any], float]:
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=json.dumps(payload, separators=(",", ":")).encode(),
            method=method,
            headers={"Content-Type": "application/json"},
        )
        start = time.perf_counter_ns()
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                data = json.loads(response.read())
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode(errors="replace")
            raise RuntimeError(f"{method} {path} returned HTTP {exc.code}: {detail}") from exc
        if not isinstance(data, dict):
            raise RuntimeError(f"{method} {path} returned non-object JSON")
        return data, (time.perf_counter_ns() - start) / 1_000_000


def _insert_memories(
    client: AgentMemoryClient,
    memories: list[dict[str, Any]],
    vectors: Any,
    *,
    run_id: str,
) -> None:
    for sample, vector in zip(memories, vectors.tolist()):
        response, _ = client.request(
            "POST",
            "/api/ai/memories",
            {
                "memory_id": f"{run_id}-{sample['episode_index']}",
                "tenant_id": f"physical-ai-027-{run_id}",
                "namespace": "libero-closed-loop",
                "agent_id": "physical-ai-vla-router",
                "type": "libero_action",
                "content": sample["task"],
                "metadata_json": json.dumps(
                    {
                        "episode_index": sample["episode_index"],
                        "task_index": sample["task_index"],
                        "action": sample["action"],
                    },
                    separators=(",", ":"),
                ),
                "embedding": vector,
                "token_count": max(1, len(sample["task"]) // 4),
                "importance": 0.0,
                "created_at_ns": sample["episode_index"] + 1,
            },
        )
        if not response.get("ok"):
            raise RuntimeError(f"ZeptoDB insert failed: {response}")


def _search(
    client: AgentMemoryClient,
    vector: Any,
    *,
    run_id: str,
    top_k: int,
) -> tuple[list[dict[str, Any]], float]:
    response, latency_ms = client.request(
        "POST",
        "/api/ai/memories/search",
        {
            "tenant_id": f"physical-ai-027-{run_id}",
            "namespace": "libero-closed-loop",
            "agent_id": "physical-ai-vla-router",
            "type": "libero_action",
            "query_embedding": vector.tolist(),
            "limit": top_k,
        },
    )
    matches = []
    for match in response.get("matches", []):
        metadata = json.loads(match["metadata_json"])
        matches.append(
            {
                "similarity": float(match["similarity"]),
                "action": list(map(float, metadata["action"])),
                "task_index": int(metadata["task_index"]),
            }
        )
    if not matches:
        raise RuntimeError("ZeptoDB returned no action matches")
    return matches, latency_ms


def _prepare_memory_bank(
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: AgentMemoryClient,
) -> tuple[dict[str, Any], dict[int, Any]]:
    import torch

    memories, _ = exp26.load_sample_manifest(
        args.sample_manifest,
        memory_per_task=args.memory_per_task,
        query_per_task=args.query_per_task,
    )
    with ThreadPoolExecutor(max_workers=args.download_workers) as pool:
        memory_images = list(
            pool.map(lambda row: exp26._fetch_image(row["front_image_url"]), memories)
        )
    state_means, state_stds = exp26._state_stats(memories)
    action_scale = exp26._action_scale(memories)
    task_texts = {int(row["task_index"]): str(row["task"]) for row in memories}
    ordered_tasks = sorted(task_texts)
    text_inputs = siglip_processor(
        text=[task_texts[index] for index in ordered_tasks],
        padding="max_length",
        return_tensors="pt",
    )
    text_inputs = {key: value.to("cuda") for key, value in text_inputs.items()}
    with torch.inference_mode():
        text_features = siglip.get_text_features(**text_inputs)
        text_features = torch.nn.functional.normalize(text_features.float(), dim=-1)
    text_by_task = {
        task_index: text_features[position]
        for position, task_index in enumerate(ordered_tasks)
    }

    batches = []
    with torch.inference_mode():
        for start in range(0, len(memories), args.batch_size):
            image_features = exp26._encode_visual_batch(
                siglip,
                siglip_processor,
                memory_images[start : start + args.batch_size],
            )
            text_batch = torch.stack(
                [
                    text_by_task[int(row["task_index"])]
                    for row in memories[start : start + args.batch_size]
                ]
            )
            batches.append(
                torch.nn.functional.normalize(image_features + text_batch, dim=-1).cpu()
            )
    visual = torch.cat(batches)
    vectors = torch.stack(
        [
            exp26._combine_embedding(vector, row["state"], state_means, state_stds)
            for vector, row in zip(visual, memories)
        ]
    )
    _insert_memories(client, memories, vectors, run_id=args.run_id)
    text_to_task = {text: index for index, text in task_texts.items()}
    return (
        {
            "memories": memories,
            "state_means": state_means,
            "state_stds": state_stds,
            "action_scale": action_scale,
            "task_texts": task_texts,
            "text_to_task": text_to_task,
        },
        text_by_task,
    )


def _policy_action(
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    env_postprocessor: Any,
    observation: dict[str, Any],
) -> tuple[Any, float, float]:
    import torch

    wall_start = time.perf_counter_ns()
    batch = preprocessor(observation)
    torch.cuda.synchronize()
    start_event = torch.cuda.Event(enable_timing=True)
    end_event = torch.cuda.Event(enable_timing=True)
    start_event.record()
    with torch.inference_mode():
        action = policy.select_action(batch)
    end_event.record()
    torch.cuda.synchronize()
    action = postprocessor(action)
    action = env_postprocessor({"action": action})["action"]
    wall_ms = (time.perf_counter_ns() - wall_start) / 1_000_000
    return action.to("cpu").numpy(), wall_ms, float(start_event.elapsed_time(end_event))


def _prior_action(prior: list[float], env_postprocessor: Any) -> Any:
    import torch

    action = torch.tensor([prior], dtype=torch.float32)
    return env_postprocessor({"action": action})["action"].to("cpu").numpy()


def _processed_observation(env: Any, raw_observation: dict[str, Any], env_preprocessor: Any) -> Any:
    from lerobot.envs.utils import add_envs_task, preprocess_observation

    observation = preprocess_observation(raw_observation)
    observation = add_envs_task(env, observation)
    return env_preprocessor(observation)


def _make_task_env(task_id: int, episode_length: int) -> tuple[Any, Any]:
    from lerobot.envs.configs import LiberoEnv as LiberoEnvConfig
    from lerobot.envs.factory import make_env

    config = LiberoEnvConfig(
        task=SUITE,
        task_ids=[task_id],
        episode_length=episode_length,
        observation_height=256,
        observation_width=256,
        control_mode="relative",
    )
    all_envs = make_env(config, n_envs=1, use_async_envs=False)
    return all_envs[SUITE][task_id], config


def _run_episode(
    *,
    variant: str,
    task_id: int,
    seed: int,
    args: argparse.Namespace,
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    siglip: Any,
    siglip_processor: Any,
    client: AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
) -> dict[str, Any]:
    import numpy as np
    from lerobot.envs.factory import make_env_pre_post_processors

    env, env_config = _make_task_env(task_id, args.episode_length)
    env_preprocessor, env_postprocessor = make_env_pre_post_processors(
        env_config, policy.config
    )
    seed_policy_rng(seed)
    policy.reset()
    raw_observation, _ = env.reset(seed=[seed])
    task_description = list(env.call("task_description"))[0]
    try:
        memory_task_index = resolve_memory_task_index(
            task_description, memory["text_to_task"]
        )
    except RuntimeError:
        env.close()
        raise

    decision_wall: list[float] = []
    policy_wall: list[float] = []
    policy_gpu: list[float] = []
    encoder_wall: list[float] = []
    encoder_gpu: list[float] = []
    search_wall: list[float] = []
    confidences: list[float] = []
    vla_calls = 0
    skips = 0
    task_mismatches = 0
    high_confidence_streak = 0
    consecutive_skips = 0
    success = False
    reward_total = 0.0
    steps = 0
    try:
        for step in range(args.episode_length):
            observation = _processed_observation(
                env, raw_observation, env_preprocessor
            )
            decision_start = time.perf_counter_ns()
            if variant == "baseline":
                action, vla_wall_ms, vla_gpu_ms = _policy_action(
                    policy,
                    preprocessor,
                    postprocessor,
                    env_postprocessor,
                    observation,
                )
                vla_calls += 1
                policy_wall.append(vla_wall_ms)
                policy_gpu.append(vla_gpu_ms)
            elif variant == "routed":
                front = observation["observation.images.image"][0].cpu()
                state = observation["observation.state"][0].tolist()
                vector, encode_wall_ms, encode_gpu_ms = exp26._encode_query(
                    siglip,
                    siglip_processor,
                    front,
                    text_by_task[memory_task_index],
                    {"state": state},
                    memory["state_means"],
                    memory["state_stds"],
                )
                matches, search_ms = _search(
                    client, vector, run_id=args.run_id, top_k=args.top_k
                )
                matching = [
                    row
                    for row in matches
                    if int(row["task_index"]) == memory_task_index
                ]
                task_match = bool(matching) and int(matches[0]["task_index"]) == memory_task_index
                if not task_match:
                    task_mismatches += 1
                if matching:
                    prior, confidence = exp26.weighted_action_prior(
                        matching, memory["action_scale"]
                    )
                else:
                    prior = [0.0] * 7
                    confidence = -math.inf
                should_skip, high_confidence_streak = route_action(
                    confidence=confidence,
                    threshold=args.threshold,
                    task_match=task_match,
                    high_confidence_streak=high_confidence_streak,
                    consecutive_skips=consecutive_skips,
                    required_streak=args.required_streak,
                    max_consecutive_skips=args.max_consecutive_skips,
                )
                if should_skip:
                    action = _prior_action(prior, env_postprocessor)
                    skips += 1
                    consecutive_skips += 1
                else:
                    action, vla_wall_ms, vla_gpu_ms = _policy_action(
                        policy,
                        preprocessor,
                        postprocessor,
                        env_postprocessor,
                        observation,
                    )
                    vla_calls += 1
                    consecutive_skips = 0
                    policy_wall.append(vla_wall_ms)
                    policy_gpu.append(vla_gpu_ms)
                encoder_wall.append(encode_wall_ms)
                encoder_gpu.append(encode_gpu_ms)
                search_wall.append(search_ms)
                if math.isfinite(confidence):
                    confidences.append(confidence)
            else:
                raise ValueError(f"unknown variant: {variant}")

            raw_observation, reward, terminated, truncated, info = env.step(action)
            reward_total += float(np.asarray(reward).reshape(-1)[0])
            steps = step + 1
            success = success or extract_success(info)
            decision_wall.append((time.perf_counter_ns() - decision_start) / 1_000_000)
            if bool(np.asarray(terminated).reshape(-1)[0]) or bool(
                np.asarray(truncated).reshape(-1)[0]
            ):
                success = success or extract_success(info)
                break
    finally:
        env.close()
    return {
        "success": success,
        "steps": steps,
        "reward": reward_total,
        "vla_calls": vla_calls,
        "skips": skips,
        "task_mismatches": task_mismatches,
        "decision_wall": decision_wall,
        "policy_wall": policy_wall,
        "policy_gpu": policy_gpu,
        "encoder_wall": encoder_wall,
        "encoder_gpu": encoder_gpu,
        "search_wall": search_wall,
        "confidences": confidences,
    }


def _energy_reader() -> Any:
    return exp26._energy_reader()


def _energy_delta(reader: Any, start: int | None) -> float | None:
    return exp26._phase_energy(reader, start)


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    siglip, siglip_processor, policy, preprocessor, postprocessor = exp26._load_models(
        args
    )
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    if int(policy.config.n_action_steps) != 1:
        raise RuntimeError("Experiment 027 requires n_action_steps=1")

    client = AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    memory, text_by_task = _prepare_memory_bank(
        args, siglip, siglip_processor, client
    )
    if args.tasks <= 0 or args.task_start < 0:
        raise ValueError("task count must be positive and task start must be non-negative")
    task_ids = list(range(args.task_start, args.task_start + args.tasks))
    if task_ids[-1] >= 10:
        raise ValueError("Experiment 027 task range exceeds LIBERO-10")

    energy = _energy_reader()
    baseline_start = energy() if energy else None
    baseline_rows = []
    for task_id in task_ids:
        baseline_rows.append(
            _run_episode(
                variant="baseline",
                task_id=task_id,
                seed=args.seed + task_id,
                args=args,
                policy=policy,
                preprocessor=preprocessor,
                postprocessor=postprocessor,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
        )
    baseline_energy = _energy_delta(energy, baseline_start)

    routed_start = energy() if energy else None
    routed_rows = []
    for task_id in task_ids:
        routed_rows.append(
            _run_episode(
                variant="routed",
                task_id=task_id,
                seed=args.seed + task_id,
                args=args,
                policy=policy,
                preprocessor=preprocessor,
                postprocessor=postprocessor,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
        )
    routed_energy = _energy_delta(energy, routed_start)

    task_results = [
        {
            "task_id": task_id,
            "baseline_success": baseline["success"],
            "routed_success": routed["success"],
            "baseline_steps": baseline["steps"],
            "routed_steps": routed["steps"],
            "routed_vla_calls": routed["vla_calls"],
            "routed_skips": routed["skips"],
        }
        for task_id, baseline, routed in zip(task_ids, baseline_rows, routed_rows)
    ]
    quality = paired_quality(task_results)
    baseline_decision = [
        value for row in baseline_rows for value in row["decision_wall"]
    ]
    baseline_gpu = [value for row in baseline_rows for value in row["policy_gpu"]]
    routed_decision = [value for row in routed_rows for value in row["decision_wall"]]
    routed_policy_gpu = [value for row in routed_rows for value in row["policy_gpu"]]
    routed_encoder_gpu = [value for row in routed_rows for value in row["encoder_gpu"]]
    routed_search = [value for row in routed_rows for value in row["search_wall"]]
    routed_confidence = [value for row in routed_rows for value in row["confidences"]]
    baseline_calls = sum(row["vla_calls"] for row in baseline_rows)
    routed_calls = sum(row["vla_calls"] for row in routed_rows)
    routed_skips = sum(row["skips"] for row in routed_rows)
    routed_decisions = routed_calls + routed_skips
    call_reduction = 1.0 - routed_calls / baseline_calls
    fallback_rate = routed_calls / routed_decisions
    latency_reduction = (
        1.0 - statistics.fmean(routed_decision) / statistics.fmean(baseline_decision)
    )
    baseline_gpu_total = sum(baseline_gpu)
    routed_gpu_total = sum(routed_policy_gpu) + sum(routed_encoder_gpu)
    gpu_reduction = 1.0 - routed_gpu_total / baseline_gpu_total
    acceptance = {
        "real_closed_loop": sum(row["steps"] for row in baseline_rows + routed_rows) > 0,
        "baseline_capable": quality["baseline_success_rate"] >= 0.10,
        "success_noninferior": quality["success_rate_delta"] >= -0.10,
        "paired_regressions": quality["paired_regressions"] <= 1,
        "vla_call_reduction": call_reduction >= 0.30,
        "fallback_exercised": 0.05 <= fallback_rate <= 0.95,
        "mean_latency_reduction": latency_reduction >= 0.15,
        "zeptodb_search_p95": percentile(routed_search, 0.95) < 30.0,
    }
    return {
        "status": "pass" if all(acceptance.values()) else "fail",
        "experiment": EXPERIMENT,
        "suite": SUITE,
        "vla_model": args.vla_model,
        "vla_sha": resolved_sha,
        "siglip_model": args.siglip_model,
        "runtime": {
            "gpu": torch.cuda.get_device_name(0),
            "cuda": torch.version.cuda,
            "peak_gpu_memory_bytes": int(torch.cuda.max_memory_allocated()),
        },
        "scope": {
            "tasks": len(task_ids),
            "init_states_per_task": 1,
            "episode_length": args.episode_length,
            "memories": len(memory["memories"]),
            "threshold": args.threshold,
            "required_streak": args.required_streak,
            "max_consecutive_skips": args.max_consecutive_skips,
        },
        "quality": quality,
        "baseline": {
            "vla_calls": baseline_calls,
            "steps": sum(row["steps"] for row in baseline_rows),
            "decision": latency_summary(baseline_decision),
            "gpu_total_ms": baseline_gpu_total,
            "energy_j": baseline_energy,
        },
        "routed": {
            "vla_calls": routed_calls,
            "skips": routed_skips,
            "steps": sum(row["steps"] for row in routed_rows),
            "call_reduction": call_reduction,
            "fallback_rate": fallback_rate,
            "decision": latency_summary(routed_decision),
            "search": latency_summary(routed_search),
            "confidence_p50": percentile(routed_confidence, 0.50),
            "task_mismatches": sum(row["task_mismatches"] for row in routed_rows),
            "gpu_total_ms": routed_gpu_total,
            "gpu_reduction": gpu_reduction,
            "latency_reduction": latency_reduction,
            "energy_j": routed_energy,
        },
        "tasks": [
            {
                "i": row["task_id"],
                "b": int(row["baseline_success"]),
                "r": int(row["routed_success"]),
                "bs": row["baseline_steps"],
                "rs": row["routed_steps"],
                "rv": row["routed_vla_calls"],
                "sk": row["routed_skips"],
            }
            for row in task_results
        ],
        "acceptance": acceptance,
    }


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    quality = result["quality"]
    baseline = result["baseline"]
    routed = result["routed"]
    scope = result["scope"]
    labels = {
        "real_closed_loop": "Real LIBERO closed-loop steps executed",
        "baseline_capable": "Direct VLA succeeds on at least one task",
        "success_noninferior": "Routed success rate no more than 10pp below direct VLA",
        "paired_regressions": "At most one paired task regression",
        "vla_call_reduction": "VLA calls reduced by at least 30%",
        "fallback_exercised": "Fallback rate is between 5% and 95%",
        "mean_latency_reduction": "Mean decision latency reduced by at least 15%",
        "zeptodb_search_p95": "ZeptoDB exact-search p95 below 30 ms",
        "aws_resources_deleted": "Temporary AWS resources deleted",
    }
    energy_baseline = (
        f"{baseline['energy_j']:.1f}" if baseline["energy_j"] is not None else "unsupported"
    )
    energy_routed = (
        f"{routed['energy_j']:.1f}" if routed["energy_j"] is not None else "unsupported"
    )
    lines = [
        "# Physical AI VLA Closed Loop 027 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Suite: `{result['suite']}`",
        f"VLA: `{result['vla_model']}` at `{result['vla_sha']}`",
        f"Retrieval encoder: `{result['siglip_model']}`",
        "",
        "## Scope",
        "",
        "Direct VLA and the ZeptoDB-routed policy ran from paired LIBERO initial",
        "states. Success is reported by the real simulator. This bounded run uses",
        "one initial state per task and is not a production safety claim.",
        "",
        "| Tasks | Init states/task | Max steps | Memories | Threshold | Guard |",
        "| ---: | ---: | ---: | ---: | ---: | --- |",
        f"| {scope['tasks']} | {scope['init_states_per_task']} | "
        f"{scope['episode_length']} | {scope['memories']} | "
        f"{scope['threshold']:.6f} | {scope['required_streak']} high-confidence, "
        f"max {scope['max_consecutive_skips']} skips |",
        "",
        "## Closed-Loop Quality",
        "",
        "| Path | Successes | Success rate | Steps |",
        "| --- | ---: | ---: | ---: |",
        f"| Direct VLA | {quality['baseline_successes']}/{quality['tasks']} | "
        f"{quality['baseline_success_rate']:.1%} | {baseline['steps']} |",
        f"| ZeptoDB routed | {quality['routed_successes']}/{quality['tasks']} | "
        f"{quality['routed_success_rate']:.1%} | {routed['steps']} |",
        "",
        f"Paired regressions: {quality['paired_regressions']}; paired improvements: "
        f"{quality['paired_improvements']}.",
        "",
        "## Compute And Latency",
        "",
        "| Path | VLA calls | Skips | Mean decision ms | p95 ms | GPU total ms | Energy J |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
        f"| Direct VLA | {baseline['vla_calls']} | 0 | "
        f"{baseline['decision']['mean_ms']:.3f} | "
        f"{baseline['decision']['p95_ms']:.3f} | "
        f"{baseline['gpu_total_ms']:.3f} | {energy_baseline} |",
        f"| ZeptoDB routed | {routed['vla_calls']} | {routed['skips']} | "
        f"{routed['decision']['mean_ms']:.3f} | "
        f"{routed['decision']['p95_ms']:.3f} | "
        f"{routed['gpu_total_ms']:.3f} | {energy_routed} |",
        "",
        f"- VLA call reduction: {routed['call_reduction']:.1%}.",
        f"- Fallback rate: {routed['fallback_rate']:.1%}.",
        f"- Mean decision-latency reduction: {routed['latency_reduction']:.1%}.",
        f"- Online GPU-time reduction: {routed['gpu_reduction']:.1%}.",
        f"- ZeptoDB search p50/p95: {routed['search']['p50_ms']:.3f}/"
        f"{routed['search']['p95_ms']:.3f} ms.",
        "",
        "## Per-Task Result",
        "",
        "| Task | Direct success | Routed success | Direct steps | Routed steps | Routed VLA | Skips |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in result["tasks"]:
        lines.append(
            f"| {row['i']} | {'yes' if row['b'] else 'no'} | "
            f"{'yes' if row['r'] else 'no'} | {row['bs']} | {row['rs']} | "
            f"{row['rv']} | {row['sk']} |"
        )
    lines += ["", "## Acceptance", "", "| Criterion | Status |", "| --- | --- |"]
    lines.extend(
        f"| {labels[key]} | {'pass' if passed else 'fail'} |"
        for key, passed in result["acceptance"].items()
    )
    lines += [
        "",
        "## Result",
        "",
        f"Overall status: {result['status']}.",
        "",
        "## Interpretation",
        "",
        "This run tests real closed-loop behavior for a bounded deterministic slice.",
        "It does not establish confidence intervals, drift robustness, collision",
        "safety, or readiness to control a physical robot.",
    ]
    return "\n".join(lines) + "\n"


def _write_result(path: Path, result: dict[str, Any]) -> None:
    encoded = json.dumps(result, separators=(",", ":"), sort_keys=True)
    if len(encoded.encode()) > 3900:
        raise RuntimeError("compact result exceeds the Kubernetes termination-message limit")
    path.write_text(encoded)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent-url", default="http://zepto-agent-memory:8123")
    parser.add_argument("--siglip-model", default=SIGLIP_MODEL)
    parser.add_argument("--vla-model", default=VLA_MODEL)
    parser.add_argument("--run-id", default=str(int(time.time())))
    parser.add_argument("--sample-manifest", type=Path)
    parser.add_argument("--memory-per-task", type=int, default=19)
    parser.add_argument("--query-per-task", type=int, default=10)
    parser.add_argument("--tasks", type=int, default=10)
    parser.add_argument("--task-start", type=int, default=0)
    parser.add_argument("--episode-length", type=int, default=520)
    parser.add_argument("--seed", type=int, default=27)
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    parser.add_argument("--required-streak", type=int, default=2)
    parser.add_argument("--max-consecutive-skips", type=int, default=4)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--download-workers", type=int, default=12)
    parser.add_argument("--http-timeout", type=float, default=15.0)
    parser.add_argument("--result", type=Path, default=Path("/dev/termination-log"))
    parser.add_argument("--render-json", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.render_json:
        result = json.loads(args.render_json.read_text())
        rendered = render_report(result)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered)
        else:
            print(rendered, end="")
        return 0
    if args.sample_manifest is None:
        parser.error("--sample-manifest is required")
    try:
        result = run_experiment(args)
        _write_result(args.result, result)
        return 0 if result["status"] == "pass" else 2
    except Exception as exc:
        _write_result(
            args.result,
            {
                "status": "error",
                "experiment": EXPERIMENT,
                "error_type": type(exc).__name__,
                "error": str(exc)[:3000],
            },
        )
        raise


if __name__ == "__main__":
    raise SystemExit(main())

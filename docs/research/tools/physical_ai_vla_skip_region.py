#!/usr/bin/env python3
"""Run research-only Experiment 028 to find safe VLA skip regions."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import physical_ai_vla_closed_loop as exp27  # noqa: E402
import physical_ai_vla_early_exit as exp26  # noqa: E402


EXPERIMENT = 28
SUITE = exp27.SUITE
VLA_MODEL = exp27.VLA_MODEL
VLA_MODEL_SHA = exp27.VLA_MODEL_SHA
SIGLIP_MODEL = exp27.SIGLIP_MODEL


def trajectory_phase(step: int, episode_length: int) -> str:
    if step < 0 or episode_length <= 0 or step >= episode_length:
        raise ValueError("step must be within a positive episode length")
    phase = min(2, (step * 3) // episode_length)
    return ("early", "middle", "late")[phase]


def simulate_skip_mask(
    rows: list[dict[str, Any]],
    *,
    threshold: float,
    min_margin: float,
    required_streak: int,
    max_consecutive_skips: int,
) -> list[bool]:
    if required_streak <= 0 or max_consecutive_skips <= 0:
        raise ValueError("router streak limits must be positive")
    mask: list[bool] = []
    current_task: int | None = None
    streak = 0
    consecutive = 0
    for row in rows:
        task_id = int(row["task_id"])
        if task_id != current_task:
            current_task = task_id
            streak = 0
            consecutive = 0
        eligible = (
            float(row["confidence"]) >= threshold
            and float(row["margin"]) >= min_margin
        )
        streak = streak + 1 if eligible else 0
        skip = streak >= required_streak and consecutive < max_consecutive_skips
        mask.append(skip)
        consecutive = consecutive + 1 if skip else 0
    return mask


def evaluate_skip_region(
    rows: list[dict[str, Any]],
    *,
    threshold: float,
    min_margin: float,
    required_streak: int,
    max_consecutive_skips: int,
) -> dict[str, float | int]:
    if not rows:
        raise ValueError("shadow rows must not be empty")
    mask = simulate_skip_mask(
        rows,
        threshold=threshold,
        min_margin=min_margin,
        required_streak=required_streak,
        max_consecutive_skips=max_consecutive_skips,
    )
    errors = [
        float(row["candidate_mae"]) for row, skip in zip(rows, mask) if skip
    ]
    baseline_latency = statistics.fmean(float(row["policy_wall"]) for row in rows)
    projected = statistics.fmean(
        float(row["encoder_wall"])
        + float(row["search_wall"])
        + (0.0 if skip else float(row["policy_wall"]))
        for row, skip in zip(rows, mask)
    )
    return {
        "threshold": threshold,
        "min_margin": min_margin,
        "skips": sum(mask),
        "skip_rate": sum(mask) / len(mask),
        "mean_action_mae": statistics.fmean(errors) if errors else math.inf,
        "p95_action_mae": exp27.percentile(errors, 0.95) if errors else math.inf,
        "projected_latency_reduction": 1.0 - projected / baseline_latency,
    }


def select_skip_region(
    rows: list[dict[str, Any]],
    *,
    threshold_min: float,
    threshold_max: float,
    threshold_step: float,
    margin_candidates: list[float],
    target_skip_min: float,
    target_skip_max: float,
    max_mean_action_mae: float,
    max_p95_action_mae: float,
    min_projected_latency_reduction: float,
    required_streak: int,
    max_consecutive_skips: int,
) -> dict[str, Any]:
    if not 0.0 <= target_skip_min <= target_skip_max <= 1.0:
        raise ValueError("target skip range must be ordered within [0, 1]")
    if threshold_step <= 0.0 or threshold_min > threshold_max:
        raise ValueError("threshold search range is invalid")
    if not margin_candidates:
        raise ValueError("at least one margin candidate is required")
    thresholds = []
    value = threshold_min
    while value <= threshold_max + threshold_step / 10.0:
        thresholds.append(round(value, 8))
        value += threshold_step
    candidates = [
        evaluate_skip_region(
            rows,
            threshold=threshold,
            min_margin=margin,
            required_streak=required_streak,
            max_consecutive_skips=max_consecutive_skips,
        )
        for threshold in thresholds
        for margin in margin_candidates
    ]
    safe = [
        row
        for row in candidates
        if float(row["mean_action_mae"]) <= max_mean_action_mae
        and float(row["p95_action_mae"]) <= max_p95_action_mae
        and float(row["projected_latency_reduction"])
        >= min_projected_latency_reduction
    ]
    viable = [
        row
        for row in safe
        if target_skip_min <= float(row["skip_rate"]) <= target_skip_max
    ]
    ranking = lambda row: (
        -float(row["skip_rate"]),
        float(row["mean_action_mae"]),
        -float(row["projected_latency_reduction"]),
        float(row["threshold"]),
        float(row["min_margin"]),
    )
    selected = min(viable, key=ranking) if viable else None
    best_safe = min(safe, key=ranking) if safe else None
    return {
        "viable": selected is not None,
        "selected": selected,
        "best_safe": best_safe,
        "candidates": len(candidates),
    }


def summarize_phases(
    rows: list[dict[str, Any]], mask: list[bool]
) -> list[dict[str, Any]]:
    if len(rows) != len(mask):
        raise ValueError("rows and skip mask must have equal length")
    result = []
    for phase in ("early", "middle", "late"):
        phase_rows = [
            (row, skip)
            for row, skip in zip(rows, mask)
            if row["phase"] == phase
        ]
        confidences = [float(row["confidence"]) for row, _ in phase_rows]
        errors = [
            float(row["candidate_mae"]) for row, skip in phase_rows if skip
        ]
        result.append(
            {
                "p": phase,
                "n": len(phase_rows),
                "sk": sum(skip for _, skip in phase_rows),
                "c50": exp27.percentile(confidences, 0.50),
                "e": statistics.fmean(errors) if errors else None,
            }
        )
    return result


def parse_task_ids(value: str) -> list[int]:
    try:
        task_ids = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise ValueError("pilot task ids must be comma-separated integers") from exc
    if not task_ids or len(set(task_ids)) != len(task_ids):
        raise ValueError("pilot task ids must be non-empty and unique")
    if any(task_id < 0 or task_id >= 10 for task_id in task_ids):
        raise ValueError("pilot task ids must be within LIBERO-10")
    return task_ids


def _namespace(task_index: int) -> str:
    return f"libero-skip-region-task-{task_index}"


def _insert_memories(
    client: exp27.AgentMemoryClient,
    memories: list[dict[str, Any]],
    vectors: Any,
    *,
    run_id: str,
) -> None:
    for sample, vector in zip(memories, vectors.tolist()):
        task_index = int(sample["task_index"])
        response, _ = client.request(
            "POST",
            "/api/ai/memories",
            {
                "memory_id": f"{run_id}-{sample['episode_index']}",
                "tenant_id": f"physical-ai-028-{run_id}",
                "namespace": _namespace(task_index),
                "agent_id": "physical-ai-vla-skip-region",
                "type": "libero_action",
                "content": sample["task"],
                "metadata_json": json.dumps(
                    {
                        "episode_index": sample["episode_index"],
                        "task_index": task_index,
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
    client: exp27.AgentMemoryClient,
    vector: Any,
    *,
    run_id: str,
    task_index: int,
    top_k: int,
) -> tuple[list[dict[str, Any]], float]:
    response, latency_ms = client.request(
        "POST",
        "/api/ai/memories/search",
        {
            "tenant_id": f"physical-ai-028-{run_id}",
            "namespace": _namespace(task_index),
            "agent_id": "physical-ai-vla-skip-region",
            "type": "libero_action",
            "query_embedding": vector.tolist(),
            "limit": top_k,
        },
    )
    matches = []
    for match in response.get("matches", []):
        metadata = json.loads(match["metadata_json"])
        match_task = int(metadata["task_index"])
        if match_task != task_index:
            raise RuntimeError(
                f"task-partition search returned task {match_task} for {task_index}"
            )
        matches.append(
            {
                "similarity": float(match["similarity"]),
                "action": list(map(float, metadata["action"])),
                "task_index": match_task,
            }
        )
    if not matches:
        raise RuntimeError("ZeptoDB returned no task-partitioned action matches")
    return matches, latency_ms


def _prepare_memory_bank(
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
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
    return (
        {
            "memories": memories,
            "state_means": state_means,
            "state_stds": state_stds,
            "action_scale": action_scale,
            "text_to_task": {text: index for index, text in task_texts.items()},
        },
        text_by_task,
    )


def _query_memory(
    *,
    observation: dict[str, Any],
    task_index: int,
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
) -> tuple[list[float], float, float, float, float, float]:
    front = observation["observation.images.image"][0].cpu()
    state = observation["observation.state"][0].tolist()
    vector, encoder_wall, encoder_gpu = exp26._encode_query(
        siglip,
        siglip_processor,
        front,
        text_by_task[task_index],
        {"state": state},
        memory["state_means"],
        memory["state_stds"],
    )
    matches, search_wall = _search(
        client,
        vector,
        run_id=args.run_id,
        task_index=task_index,
        top_k=args.top_k,
    )
    prior, confidence = exp26.weighted_action_prior(
        matches, memory["action_scale"]
    )
    margin = (
        float(matches[0]["similarity"]) - float(matches[1]["similarity"])
        if len(matches) > 1
        else 0.0
    )
    return prior, confidence, margin, encoder_wall, encoder_gpu, search_wall


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
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
    threshold: float | None = None,
    min_margin: float | None = None,
) -> dict[str, Any]:
    import numpy as np
    from lerobot.envs.factory import make_env_pre_post_processors

    env, env_config = exp27._make_task_env(task_id, args.episode_length)
    env_preprocessor, env_postprocessor = make_env_pre_post_processors(
        env_config, policy.config
    )
    exp27.seed_policy_rng(seed)
    policy.reset()
    raw_observation, _ = env.reset(seed=[seed])
    task_description = list(env.call("task_description"))[0]
    task_index = exp27.resolve_memory_task_index(
        task_description, memory["text_to_task"]
    )
    traces: list[dict[str, Any]] = []
    decision_wall: list[float] = []
    policy_gpu: list[float] = []
    encoder_gpu: list[float] = []
    search_wall: list[float] = []
    vla_calls = 0
    skips = 0
    streak = 0
    consecutive = 0
    success = False
    steps = 0
    reward_total = 0.0
    try:
        for step in range(args.episode_length):
            observation = exp27._processed_observation(
                env, raw_observation, env_preprocessor
            )
            decision_start = time.perf_counter_ns()
            prior, confidence, margin, encoder_wall, encode_gpu, search_ms = (
                _query_memory(
                    observation=observation,
                    task_index=task_index,
                    args=args,
                    siglip=siglip,
                    siglip_processor=siglip_processor,
                    client=client,
                    memory=memory,
                    text_by_task=text_by_task,
                )
            )
            candidate_action = exp27._prior_action(
                prior, env_postprocessor
            ).reshape(-1).tolist()
            if variant == "shadow":
                action, policy_wall, policy_gpu_ms = exp27._policy_action(
                    policy,
                    preprocessor,
                    postprocessor,
                    env_postprocessor,
                    observation,
                )
                vla_calls += 1
                policy_gpu.append(policy_gpu_ms)
                traces.append(
                    {
                        "task_id": task_id,
                        "step": step,
                        "phase": trajectory_phase(step, args.episode_length),
                        "confidence": confidence,
                        "margin": margin,
                        "candidate_mae": exp26.normalized_action_mae(
                            candidate_action,
                            action.reshape(-1).tolist(),
                            memory["action_scale"],
                        ),
                        "policy_wall": policy_wall,
                        "encoder_wall": encoder_wall,
                        "search_wall": search_ms,
                    }
                )
            elif variant == "routed":
                if threshold is None or min_margin is None:
                    raise ValueError("routed episode requires a frozen region")
                eligible = confidence >= threshold and margin >= min_margin
                streak = streak + 1 if eligible else 0
                should_skip = (
                    streak >= args.required_streak
                    and consecutive < args.max_consecutive_skips
                )
                if should_skip:
                    action = exp27._prior_action(prior, env_postprocessor)
                    skips += 1
                    consecutive += 1
                else:
                    action, _, policy_gpu_ms = exp27._policy_action(
                        policy,
                        preprocessor,
                        postprocessor,
                        env_postprocessor,
                        observation,
                    )
                    vla_calls += 1
                    policy_gpu.append(policy_gpu_ms)
                    consecutive = 0
            else:
                raise ValueError(f"unknown episode variant: {variant}")
            encoder_gpu.append(encode_gpu)
            search_wall.append(search_ms)
            decision_wall.append(
                (time.perf_counter_ns() - decision_start) / 1_000_000
            )
            raw_observation, reward, terminated, truncated, info = env.step(action)
            reward_total += float(np.asarray(reward).reshape(-1)[0])
            steps = step + 1
            success = success or exp27.extract_success(info)
            if bool(np.asarray(terminated).reshape(-1)[0]) or bool(
                np.asarray(truncated).reshape(-1)[0]
            ):
                success = success or exp27.extract_success(info)
                break
    finally:
        env.close()
    return {
        "task_id": task_id,
        "success": success,
        "steps": steps,
        "reward": reward_total,
        "vla_calls": vla_calls,
        "skips": skips,
        "traces": traces,
        "decision_wall": decision_wall,
        "policy_gpu": policy_gpu,
        "encoder_gpu": encoder_gpu,
        "search_wall": search_wall,
    }


def _pilot_gate(
    shadow_rows: list[dict[str, Any]],
    routed_rows: list[dict[str, Any]],
    *,
    min_skip_rate: float,
    min_latency_reduction: float,
) -> dict[str, Any]:
    regressions = sum(
        shadow["success"] and not routed["success"]
        for shadow, routed in zip(shadow_rows, routed_rows)
    )
    decisions = sum(row["vla_calls"] + row["skips"] for row in routed_rows)
    skips = sum(row["skips"] for row in routed_rows)
    baseline_latency = statistics.fmean(
        trace["policy_wall"]
        for row in shadow_rows
        for trace in row["traces"]
    )
    routed_latency = statistics.fmean(
        value for row in routed_rows for value in row["decision_wall"]
    )
    skip_rate = skips / decisions
    latency_reduction = 1.0 - routed_latency / baseline_latency
    checks = {
        "no_regression": regressions == 0,
        "skip_rate": skip_rate >= min_skip_rate,
        "latency_reduction": latency_reduction >= min_latency_reduction,
    }
    return {
        "pass": all(checks.values()),
        "regressions": regressions,
        "skip_rate": skip_rate,
        "latency_reduction": latency_reduction,
        "checks": checks,
    }


def _compact_failure(
    args: argparse.Namespace,
    *,
    resolved_sha: str,
    reason: str,
    calibration: dict[str, Any],
    shadow_rows: list[dict[str, Any]],
) -> dict[str, Any]:
    traces = [trace for row in shadow_rows for trace in row["traces"]]
    return {
        "status": "fail",
        "experiment": EXPERIMENT,
        "suite": SUITE,
        "vla_model": args.vla_model,
        "vla_sha": resolved_sha,
        "siglip_model": args.siglip_model,
        "stop_reason": reason,
        "scope": {
            "tasks": len(shadow_rows),
            "memories": args.memory_per_task * 10,
            "episode_length": args.episode_length,
        },
        "calibration": calibration,
        "shadow": {
            "steps": len(traces),
            "successes": sum(row["success"] for row in shadow_rows),
            "search": exp27.latency_summary(
                [trace["search_wall"] for trace in traces]
            ),
        },
        "measurement": {"decision_timer_scope": "policy_plus_retrieval"},
        "acceptance": {
            "task_partitioned": True,
            "viable_skip_region": bool(calibration["viable"]),
            "pilot_passed": False,
        },
    }


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    pilot_task_ids = parse_task_ids(args.pilot_task_ids)
    siglip, siglip_processor, policy, preprocessor, postprocessor = exp26._load_models(
        args
    )
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    if int(policy.config.n_action_steps) != 1:
        raise RuntimeError("Experiment 028 requires n_action_steps=1")
    client = exp27.AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    memory, text_by_task = _prepare_memory_bank(
        args, siglip, siglip_processor, client
    )

    def run(variant: str, task_id: int, **region: float) -> dict[str, Any]:
        return _run_episode(
            variant=variant,
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
            **region,
        )

    pilot_shadow = [run("shadow", task_id) for task_id in pilot_task_ids]
    pilot_traces = [trace for row in pilot_shadow for trace in row["traces"]]
    calibration = select_skip_region(
        pilot_traces,
        threshold_min=args.threshold_min,
        threshold_max=args.threshold_max,
        threshold_step=args.threshold_step,
        margin_candidates=args.margin_candidates,
        target_skip_min=args.target_skip_min,
        target_skip_max=args.target_skip_max,
        max_mean_action_mae=args.max_mean_action_mae,
        max_p95_action_mae=args.max_p95_action_mae,
        min_projected_latency_reduction=args.min_latency_reduction,
        required_streak=args.required_streak,
        max_consecutive_skips=args.max_consecutive_skips,
    )
    if not calibration["viable"]:
        return _compact_failure(
            args,
            resolved_sha=resolved_sha,
            reason="no_viable_shadow_skip_region",
            calibration=calibration,
            shadow_rows=pilot_shadow,
        )
    selected = calibration["selected"]
    threshold = float(selected["threshold"])
    min_margin = float(selected["min_margin"])
    pilot_routed = [
        run(
            "routed",
            task_id,
            threshold=threshold,
            min_margin=min_margin,
        )
        for task_id in pilot_task_ids
    ]
    pilot = _pilot_gate(
        pilot_shadow,
        pilot_routed,
        min_skip_rate=args.target_skip_min,
        min_latency_reduction=args.min_latency_reduction,
    )
    if not pilot["pass"]:
        failure = _compact_failure(
            args,
            resolved_sha=resolved_sha,
            reason="closed_loop_pilot_gate_failed",
            calibration=calibration,
            shadow_rows=pilot_shadow,
        )
        failure["pilot"] = pilot
        return failure

    remaining = [task_id for task_id in range(10) if task_id not in pilot_task_ids]
    shadow_rows = pilot_shadow + [run("shadow", task_id) for task_id in remaining]
    routed_rows = pilot_routed + [
        run(
            "routed",
            task_id,
            threshold=threshold,
            min_margin=min_margin,
        )
        for task_id in remaining
    ]
    shadow_by_task = {row["task_id"]: row for row in shadow_rows}
    routed_by_task = {row["task_id"]: row for row in routed_rows}
    ordered_shadow = [shadow_by_task[index] for index in range(10)]
    ordered_routed = [routed_by_task[index] for index in range(10)]
    task_results = [
        {
            "task_id": task_id,
            "baseline_success": shadow["success"],
            "routed_success": routed["success"],
            "baseline_steps": shadow["steps"],
            "routed_steps": routed["steps"],
            "routed_vla_calls": routed["vla_calls"],
            "routed_skips": routed["skips"],
        }
        for task_id, shadow, routed in zip(
            range(10), ordered_shadow, ordered_routed
        )
    ]
    quality = exp27.paired_quality(task_results)
    all_traces = [trace for row in ordered_shadow for trace in row["traces"]]
    shadow_mask = simulate_skip_mask(
        all_traces,
        threshold=threshold,
        min_margin=min_margin,
        required_streak=args.required_streak,
        max_consecutive_skips=args.max_consecutive_skips,
    )
    routed_decisions = sum(
        row["vla_calls"] + row["skips"] for row in ordered_routed
    )
    routed_skips = sum(row["skips"] for row in ordered_routed)
    skip_rate = routed_skips / routed_decisions
    baseline_latency = statistics.fmean(
        trace["policy_wall"] for trace in all_traces
    )
    routed_latency_values = [
        value for row in ordered_routed for value in row["decision_wall"]
    ]
    latency_reduction = 1.0 - statistics.fmean(
        routed_latency_values
    ) / baseline_latency
    baseline_gpu_per_decision = statistics.fmean(
        value for row in ordered_shadow for value in row["policy_gpu"]
    )
    routed_gpu_per_decision = (
        sum(value for row in ordered_routed for value in row["policy_gpu"])
        + sum(value for row in ordered_routed for value in row["encoder_gpu"])
    ) / routed_decisions
    gpu_reduction = 1.0 - routed_gpu_per_decision / baseline_gpu_per_decision
    searches = [value for row in ordered_routed for value in row["search_wall"]]
    acceptance = {
        "task_partitioned": True,
        "viable_skip_region": True,
        "pilot_passed": True,
        "success_noninferior": quality["success_rate_delta"] >= -0.10,
        "paired_regressions": quality["paired_regressions"] <= 1,
        "skip_rate": skip_rate >= args.target_skip_min,
        "latency_reduction": latency_reduction >= args.min_latency_reduction,
        "search_p95": exp27.percentile(searches, 0.95) < 30.0,
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
        "measurement": {"decision_timer_scope": "policy_plus_retrieval"},
        "scope": {
            "tasks": 10,
            "pilot_tasks": pilot_task_ids,
            "memories": len(memory["memories"]),
            "episode_length": args.episode_length,
            "required_streak": args.required_streak,
            "max_consecutive_skips": args.max_consecutive_skips,
        },
        "region": {
            "threshold": threshold,
            "min_margin": min_margin,
            "shadow_skip_rate": selected["skip_rate"],
            "shadow_mean_mae": selected["mean_action_mae"],
            "shadow_p95_mae": selected["p95_action_mae"],
            "shadow_projected_latency_reduction": selected[
                "projected_latency_reduction"
            ],
        },
        "phases": summarize_phases(all_traces, shadow_mask),
        "pilot": pilot,
        "quality": quality,
        "baseline": {
            "steps": sum(row["steps"] for row in ordered_shadow),
            "vla_calls": sum(row["vla_calls"] for row in ordered_shadow),
            "mean_decision_ms": baseline_latency,
            "gpu_per_decision_ms": baseline_gpu_per_decision,
        },
        "routed": {
            "steps": sum(row["steps"] for row in ordered_routed),
            "vla_calls": sum(row["vla_calls"] for row in ordered_routed),
            "skips": routed_skips,
            "skip_rate": skip_rate,
            "fallback_rate": 1.0 - skip_rate,
            "decision": exp27.latency_summary(routed_latency_values),
            "latency_reduction": latency_reduction,
            "gpu_per_decision_ms": routed_gpu_per_decision,
            "gpu_reduction": gpu_reduction,
            "search": exp27.latency_summary(searches),
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
    lines = [
        "# Physical AI VLA Skip Region 028 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Status: {result['status']}",
        f"Suite: `{result['suite']}`",
        f"VLA: `{result['vla_model']}` at `{result['vla_sha']}`",
        f"Retrieval encoder: `{result['siglip_model']}`",
        "",
        "## Result",
        "",
    ]
    if result.get("stop_reason"):
        calibration = result["calibration"]
        selected = calibration.get("selected")
        lines += [
            f"The staged run stopped at `{result['stop_reason']}`.",
            "",
            f"Shadow steps: {result['shadow']['steps']}; successful pilot baselines: "
            f"{result['shadow']['successes']}/{result['scope']['tasks']}.",
            f"ZeptoDB search p95: {result['shadow']['search']['p95_ms']:.3f} ms.",
        ]
        if selected:
            lines += [
                "",
                "## Shadow Region",
                "",
                "| Confidence | Min margin | Skip rate | Mean action MAE | p95 action MAE | Projected latency reduction |",
                "| ---: | ---: | ---: | ---: | ---: | ---: |",
                f"| {selected['threshold']:.3f} | {selected['min_margin']:.3f} | "
                f"{selected['skip_rate']:.1%} | {selected['mean_action_mae']:.4f} | "
                f"{selected['p95_action_mae']:.4f} | "
                f"{selected['projected_latency_reduction']:.1%} |",
            ]
        if result.get("pilot"):
            pilot = result["pilot"]
            lines += [
                "",
                "## Closed-Loop Pilot",
                "",
                f"- Paired regressions: {pilot['regressions']}.",
                f"- Actual skip rate: {pilot['skip_rate']:.1%}.",
            ]
            if result.get("measurement", {}).get("decision_timer_scope") == (
                "policy_plus_retrieval"
            ):
                lines.append(
                    f"- Actual latency reduction: {pilot['latency_reduction']:.1%}."
                )
            else:
                lines.append(
                    "- Actual latency reduction: not comparable; this run's routed "
                    "timer included the simulator step while the shadow baseline did not."
                )
        lines += [
            "",
            "No full ten-task routed run was started after the failed gate.",
        ]
        return "\n".join(lines) + "\n"

    quality = result["quality"]
    region = result["region"]
    baseline = result["baseline"]
    routed = result["routed"]
    scope = result["scope"]
    lines += [
        "A task-partitioned shadow calibration found a candidate skip region,",
        "passed the two-task closed-loop gate, and completed all ten paired tasks.",
        "",
        "## Selected Region",
        "",
        "| Confidence | Min margin | Shadow skip | Mean action MAE | p95 action MAE | Projected latency reduction |",
        "| ---: | ---: | ---: | ---: | ---: | ---: |",
        f"| {region['threshold']:.3f} | {region['min_margin']:.3f} | "
        f"{region['shadow_skip_rate']:.1%} | {region['shadow_mean_mae']:.4f} | "
        f"{region['shadow_p95_mae']:.4f} | "
        f"{region['shadow_projected_latency_reduction']:.1%} |",
        "",
        f"Guard: {scope['required_streak']} consecutive eligible observations, "
        f"maximum {scope['max_consecutive_skips']} consecutive skips.",
        "",
        "## Trajectory Phases",
        "",
        "| Phase | Shadow observations | Eligible skips | Skip rate | Confidence p50 | Eligible action MAE |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for phase in result["phases"]:
        error = "n/a" if phase["e"] is None else f"{phase['e']:.4f}"
        rate = phase["sk"] / phase["n"] if phase["n"] else 0.0
        lines.append(
            f"| {phase['p']} | {phase['n']} | {phase['sk']} | {rate:.1%} | "
            f"{phase['c50']:.4f} | {error} |"
        )
    lines += [
        "",
        "## Closed-Loop Result",
        "",
        "| Path | Success | Steps | VLA calls | Skips | Mean decision ms | GPU ms/decision |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        f"| Direct shadow | {quality['baseline_successes']}/{quality['tasks']} | "
        f"{baseline['steps']} | {baseline['vla_calls']} | 0 | "
        f"{baseline['mean_decision_ms']:.3f} | "
        f"{baseline['gpu_per_decision_ms']:.3f} |",
        f"| Routed | {quality['routed_successes']}/{quality['tasks']} | "
        f"{routed['steps']} | {routed['vla_calls']} | {routed['skips']} | "
        f"{routed['decision']['mean_ms']:.3f} | "
        f"{routed['gpu_per_decision_ms']:.3f} |",
        "",
        f"- Actual skip rate: {routed['skip_rate']:.1%}.",
        f"- Actual latency reduction: {routed['latency_reduction']:.1%}.",
        f"- Actual GPU-time reduction per decision: {routed['gpu_reduction']:.1%}.",
        f"- Paired regressions: {quality['paired_regressions']}.",
        f"- ZeptoDB search p50/p95: {routed['search']['p50_ms']:.3f}/"
        f"{routed['search']['p95_ms']:.3f} ms.",
        "",
        "## Per-Task Result",
        "",
        "| Task | Direct | Routed | Direct steps | Routed steps | Routed VLA | Skips |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in result["tasks"]:
        lines.append(
            f"| {row['i']} | {'yes' if row['b'] else 'no'} | "
            f"{'yes' if row['r'] else 'no'} | {row['bs']} | {row['rs']} | "
            f"{row['rv']} | {row['sk']} |"
        )
    lines += [
        "",
        "## Acceptance",
        "",
        "| Check | Status |",
        "| --- | --- |",
    ]
    lines.extend(
        f"| {key.replace('_', ' ')} | {'pass' if value else 'fail'} |"
        for key, value in result["acceptance"].items()
    )
    lines += [
        "",
        "## Interpretation",
        "",
        "This bounded run identifies where historical actions may replace VLA calls",
        "for one deterministic initial state per LIBERO-10 task. It is not a",
        "multi-seed robustness result or evidence for physical-robot deployment.",
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
    parser.add_argument("--episode-length", type=int, default=520)
    parser.add_argument("--seed", type=int, default=28)
    parser.add_argument("--pilot-task-ids", default="0,5")
    parser.add_argument("--threshold-min", type=float, default=0.68)
    parser.add_argument("--threshold-max", type=float, default=0.82)
    parser.add_argument("--threshold-step", type=float, default=0.01)
    parser.add_argument(
        "--margin-candidates",
        type=lambda value: [float(item) for item in value.split(",")],
        default=[0.0, 0.005, 0.01, 0.02],
    )
    parser.add_argument("--target-skip-min", type=float, default=0.20)
    parser.add_argument("--target-skip-max", type=float, default=0.30)
    parser.add_argument("--max-mean-action-mae", type=float, default=0.25)
    parser.add_argument("--max-p95-action-mae", type=float, default=0.75)
    parser.add_argument("--min-latency-reduction", type=float, default=0.15)
    parser.add_argument("--required-streak", type=int, default=2)
    parser.add_argument("--max-consecutive-skips", type=int, default=2)
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

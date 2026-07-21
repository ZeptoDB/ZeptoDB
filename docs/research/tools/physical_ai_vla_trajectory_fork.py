#!/usr/bin/env python3
"""Run research-only Experiment 029 with paired LIBERO trajectory forks."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import physical_ai_vla_closed_loop as exp27  # noqa: E402
import physical_ai_vla_early_exit as exp26  # noqa: E402
import physical_ai_vla_skip_region as exp28  # noqa: E402


EXPERIMENT = 29
SUITE = exp27.SUITE
VLA_MODEL = exp27.VLA_MODEL
VLA_MODEL_SHA = exp27.VLA_MODEL_SHA
SIGLIP_MODEL = exp27.SIGLIP_MODEL


def parse_regions(value: str) -> list[dict[str, float | str]]:
    regions = []
    names: set[str] = set()
    for item in value.split(","):
        parts = item.strip().split(":")
        if len(parts) != 3:
            raise ValueError("regions must use name:threshold:margin")
        name, threshold_text, margin_text = parts
        if not name or name in names:
            raise ValueError("region names must be non-empty and unique")
        threshold = float(threshold_text)
        margin = float(margin_text)
        if not math.isfinite(threshold) or not math.isfinite(margin):
            raise ValueError("region thresholds and margins must be finite")
        names.add(name)
        regions.append(
            {"name": name, "threshold": threshold, "min_margin": margin}
        )
    if not regions:
        raise ValueError("at least one region is required")
    return regions


def update_region_streaks(
    streaks: dict[str, int],
    regions: list[dict[str, float | str]],
    *,
    confidence: float,
    margin: float,
) -> dict[str, int]:
    if {str(region["name"]) for region in regions} != set(streaks):
        raise ValueError("streak keys must match region names")
    return {
        str(region["name"]): (
            streaks[str(region["name"])] + 1
            if confidence >= float(region["threshold"])
            and margin >= float(region["min_margin"])
            else 0
        )
        for region in regions
    }


def normalized_state_mae(
    control: list[float], branch: list[float], scale: list[float]
) -> float:
    if not control or len(control) != len(branch) or len(branch) != len(scale):
        raise ValueError("state and scale dimensions must be equal and non-empty")
    if any(value <= 0.0 for value in scale):
        raise ValueError("state scale must be positive")
    return statistics.fmean(
        abs(float(left) - float(right)) / float(width)
        for left, right, width in zip(control, branch, scale)
    )


def pearson_correlation(left: list[float], right: list[float]) -> float | None:
    if len(left) != len(right) or len(left) < 2:
        return None
    left_mean = statistics.fmean(left)
    right_mean = statistics.fmean(right)
    numerator = sum(
        (x - left_mean) * (y - right_mean) for x, y in zip(left, right)
    )
    left_energy = sum((x - left_mean) ** 2 for x in left)
    right_energy = sum((y - right_mean) ** 2 for y in right)
    denominator = math.sqrt(left_energy * right_energy)
    return numerator / denominator if denominator > 0.0 else None


def _mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def summarize_forks(forks: list[dict[str, Any]]) -> dict[str, Any]:
    if not forks:
        raise ValueError("fork results must not be empty")
    points = [point for fork in forks for point in fork["points"]]
    if not points:
        raise ValueError("fork results must contain post-action points")
    action_errors = [float(fork["action_mae"]) for fork in forks]
    fork_state = [
        _mean([float(point["state_mae"]) for point in fork["points"]])
        for fork in forks
    ]
    control_eligible = sum(bool(point["control_eligible"]) for point in points)
    branch_eligible = sum(bool(point["branch_eligible"]) for point in points)
    confidence_deltas = [
        float(point["branch_confidence"]) - float(point["control_confidence"])
        for point in points
    ]
    count = len(points)
    overall = {
        "forks": len(forks),
        "tasks": len({int(fork["task_id"]) for fork in forks}),
        "phases": len({str(fork["phase"]) for fork in forks}),
        "mean_action_mae": _mean(action_errors),
        "state_mae_p50": exp27.percentile(
            [float(point["state_mae"]) for point in points], 0.50
        ),
        "pixel_mae_p50": exp27.percentile(
            [float(point["pixel_mae"]) for point in points], 0.50
        ),
        "mean_confidence_delta": _mean(confidence_deltas),
        "control_eligibility": control_eligible / count,
        "branch_eligibility": branch_eligible / count,
        "eligibility_drop": (control_eligible - branch_eligible) / count,
        "action_state_correlation": pearson_correlation(
            action_errors, fork_state
        ),
        "restore_state_max": max(float(fork["restore_state_mae"]) for fork in forks),
        "restore_pixel_max": max(float(fork["restore_pixel_mae"]) for fork in forks),
    }
    per_region = []
    for name in sorted({str(fork["region"]) for fork in forks}):
        selected = [fork for fork in forks if fork["region"] == name]
        selected_points = [
            point for fork in selected for point in fork["points"]
        ]
        control_count = sum(
            bool(point["control_eligible"]) for point in selected_points
        )
        branch_count = sum(
            bool(point["branch_eligible"]) for point in selected_points
        )
        per_region.append(
            {
                "r": name,
                "n": len(selected),
                "a": _mean([float(fork["action_mae"]) for fork in selected]),
                "s": _mean(
                    [
                        float(point["state_mae"])
                        for point in selected_points
                    ]
                ),
                "p": _mean(
                    [
                        float(point["pixel_mae"])
                        for point in selected_points
                    ]
                ),
                "cd": _mean(
                    [
                        float(point["branch_confidence"])
                        - float(point["control_confidence"])
                        for point in selected_points
                    ]
                ),
                "ce": control_count / len(selected_points),
                "be": branch_count / len(selected_points),
            }
        )
    return {"overall": overall, "regions": per_region}


def assess_cause(
    summary: dict[str, Any],
    *,
    min_forks: int,
    min_state_divergence: float,
    min_eligibility_drop: float,
    min_confidence_drop: float,
    min_correlation: float,
) -> dict[str, bool]:
    overall = summary["overall"]
    correlation = overall["action_state_correlation"]
    return {
        "coverage": (
            int(overall["forks"]) >= min_forks
            and int(overall["tasks"]) >= 2
            and int(overall["phases"]) >= 2
        ),
        "restore_exact": (
            float(overall["restore_state_max"]) <= 1e-8
            and float(overall["restore_pixel_max"]) <= 0.0
        ),
        "trajectory_shift": (
            float(overall["state_mae_p50"]) >= min_state_divergence
        ),
        "eligibility_effect": (
            float(overall["eligibility_drop"]) >= min_eligibility_drop
            or float(overall["mean_confidence_delta"]) <= -min_confidence_drop
        ),
        "dose_response": (
            correlation is not None and float(correlation) >= min_correlation
        ),
    }


def _batch_observation(value: Any) -> Any:
    import numpy as np

    if isinstance(value, dict):
        return {key: _batch_observation(item) for key, item in value.items()}
    return np.expand_dims(np.asarray(value), axis=0)


def _restore_observation(env: Any, snapshot: Any) -> dict[str, Any]:
    inner = env.envs[0]
    raw = inner._env.regenerate_obs_from_state(snapshot)
    return _batch_observation(inner._format_raw_obs(raw))


def _pixel_mae(control: dict[str, Any], branch: dict[str, Any]) -> float:
    import torch

    left = control["observation.images.image"].float()
    right = branch["observation.images.image"].float()
    return float(torch.mean(torch.abs(left - right)).item())


def _region_eligible(
    confidence: float, margin: float, region: dict[str, float | str]
) -> bool:
    return confidence >= float(region["threshold"]) and margin >= float(
        region["min_margin"]
    )


def _run_shadow_targets(
    *,
    task_id: int,
    seed: int,
    regions: list[dict[str, float | str]],
    args: argparse.Namespace,
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
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
    streaks = {str(region["name"]): 0 for region in regions}
    targets: list[dict[str, Any]] = []
    selected: set[tuple[str, str]] = set()
    searches: list[float] = []
    success = False
    steps = 0
    try:
        for step in range(args.episode_length):
            observation = exp27._processed_observation(
                env, raw_observation, env_preprocessor
            )
            prior, confidence, margin, _, _, search_ms = exp28._query_memory(
                observation=observation,
                task_index=task_index,
                args=args,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
            searches.append(search_ms)
            action, _, _ = exp27._policy_action(
                policy,
                preprocessor,
                postprocessor,
                env_postprocessor,
                observation,
            )
            candidate = exp27._prior_action(
                prior, env_postprocessor
            ).reshape(-1).tolist()
            phase = exp28.trajectory_phase(step, args.episode_length)
            streaks = update_region_streaks(
                streaks,
                regions,
                confidence=confidence,
                margin=margin,
            )
            for region in regions:
                name = str(region["name"])
                key = (name, phase)
                if (
                    key not in selected
                    and streaks[name] >= args.required_streak
                ):
                    selected.add(key)
                    targets.append(
                        {
                            "task_id": task_id,
                            "seed": seed,
                            "step": step,
                            "phase": phase,
                            "region": name,
                            "threshold": float(region["threshold"]),
                            "min_margin": float(region["min_margin"]),
                            "snapshot": np.asarray(
                                env.envs[0]._env.get_sim_state()
                            ).copy(),
                            "control_action": action.reshape(-1).tolist(),
                            "branch_action": candidate,
                            "confidence": confidence,
                            "margin": margin,
                        }
                    )
            raw_observation, _, terminated, truncated, info = env.step(action)
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
        "targets": targets,
        "searches": searches,
    }


def _run_fork(
    *,
    target: dict[str, Any],
    region: dict[str, float | str],
    args: argparse.Namespace,
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
) -> dict[str, Any]:
    import numpy as np
    from lerobot.envs.factory import make_env_pre_post_processors

    control_env, env_config = exp27._make_task_env(
        int(target["task_id"]), args.episode_length
    )
    branch_env, _ = exp27._make_task_env(
        int(target["task_id"]), args.episode_length
    )
    control_pre, control_post = make_env_pre_post_processors(
        env_config, policy.config
    )
    branch_pre, branch_post = make_env_pre_post_processors(
        env_config, policy.config
    )
    try:
        control_env.reset(seed=[int(target["seed"])])
        branch_env.reset(seed=[int(target["seed"])])
        control_raw = _restore_observation(control_env, target["snapshot"])
        branch_raw = _restore_observation(branch_env, target["snapshot"])
        control_obs = exp27._processed_observation(
            control_env, control_raw, control_pre
        )
        branch_obs = exp27._processed_observation(
            branch_env, branch_raw, branch_pre
        )
        restore_state = normalized_state_mae(
            control_obs["observation.state"][0].tolist(),
            branch_obs["observation.state"][0].tolist(),
            memory["state_stds"],
        )
        restore_pixel = _pixel_mae(control_obs, branch_obs)
        control_action = np.asarray([target["control_action"]], dtype=np.float32)
        branch_action = np.asarray([target["branch_action"]], dtype=np.float32)
        action_mae = exp26.normalized_action_mae(
            list(map(float, target["branch_action"])),
            list(map(float, target["control_action"])),
            memory["action_scale"],
        )
        control_raw, _, control_term, control_trunc, _ = control_env.step(
            control_action
        )
        branch_raw, _, branch_term, branch_trunc, _ = branch_env.step(
            branch_action
        )
        points = []
        searches = []
        for offset in range(1, args.fork_horizon + 1):
            control_obs = exp27._processed_observation(
                control_env, control_raw, control_pre
            )
            branch_obs = exp27._processed_observation(
                branch_env, branch_raw, branch_pre
            )
            control_query = exp28._query_memory(
                observation=control_obs,
                task_index=exp27.resolve_memory_task_index(
                    list(control_env.call("task_description"))[0],
                    memory["text_to_task"],
                ),
                args=args,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
            branch_query = exp28._query_memory(
                observation=branch_obs,
                task_index=exp27.resolve_memory_task_index(
                    list(branch_env.call("task_description"))[0],
                    memory["text_to_task"],
                ),
                args=args,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
            _, control_conf, control_margin, _, _, control_search = control_query
            _, branch_conf, branch_margin, _, _, branch_search = branch_query
            searches.extend([control_search, branch_search])
            points.append(
                {
                    "offset": offset,
                    "state_mae": normalized_state_mae(
                        control_obs["observation.state"][0].tolist(),
                        branch_obs["observation.state"][0].tolist(),
                        memory["state_stds"],
                    ),
                    "pixel_mae": _pixel_mae(control_obs, branch_obs),
                    "control_confidence": control_conf,
                    "branch_confidence": branch_conf,
                    "control_eligible": _region_eligible(
                        control_conf, control_margin, region
                    ),
                    "branch_eligible": _region_eligible(
                        branch_conf, branch_margin, region
                    ),
                }
            )
            if offset == args.fork_horizon:
                break
            if any(
                bool(np.asarray(value).reshape(-1)[0])
                for value in (
                    control_term,
                    control_trunc,
                    branch_term,
                    branch_trunc,
                )
            ):
                break
            policy_seed = (
                args.policy_fork_seed
                + int(target["task_id"]) * 1000
                + int(target["step"]) * 10
                + offset
            )
            exp27.seed_policy_rng(policy_seed)
            policy.reset()
            control_action, _, _ = exp27._policy_action(
                policy,
                preprocessor,
                postprocessor,
                control_post,
                control_obs,
            )
            exp27.seed_policy_rng(policy_seed)
            policy.reset()
            branch_action, _, _ = exp27._policy_action(
                policy,
                preprocessor,
                postprocessor,
                branch_post,
                branch_obs,
            )
            control_raw, _, control_term, control_trunc, _ = control_env.step(
                control_action
            )
            branch_raw, _, branch_term, branch_trunc, _ = branch_env.step(
                branch_action
            )
    finally:
        control_env.close()
        branch_env.close()
    return {
        "task_id": int(target["task_id"]),
        "step": int(target["step"]),
        "phase": str(target["phase"]),
        "region": str(target["region"]),
        "action_mae": action_mae,
        "restore_state_mae": restore_state,
        "restore_pixel_mae": restore_pixel,
        "points": points,
        "searches": searches,
    }


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    regions = parse_regions(args.regions)
    task_ids = exp28.parse_task_ids(args.task_ids)
    siglip, siglip_processor, policy, preprocessor, postprocessor = exp26._load_models(
        args
    )
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    if int(policy.config.n_action_steps) != 1:
        raise RuntimeError("Experiment 029 requires n_action_steps=1")
    client = exp27.AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    memory, text_by_task = exp28._prepare_memory_bank(
        args, siglip, siglip_processor, client
    )
    shadow_rows = [
        _run_shadow_targets(
            task_id=task_id,
            seed=args.seed + task_id,
            regions=regions,
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
        for task_id in task_ids
    ]
    targets = [target for row in shadow_rows for target in row["targets"]]
    if len(targets) < args.min_forks:
        return {
            "status": "fail",
            "experiment": EXPERIMENT,
            "stop_reason": "insufficient_fork_targets",
            "target_count": len(targets),
            "required": args.min_forks,
            "tasks": task_ids,
            "acceptance": {"coverage": False},
        }
    region_by_name = {str(region["name"]): region for region in regions}
    forks = [
        _run_fork(
            target=target,
            region=region_by_name[str(target["region"])],
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
        for target in targets
    ]
    summary = summarize_forks(forks)
    acceptance = assess_cause(
        summary,
        min_forks=args.min_forks,
        min_state_divergence=args.min_state_divergence,
        min_eligibility_drop=args.min_eligibility_drop,
        min_confidence_drop=args.min_confidence_drop,
        min_correlation=args.min_correlation,
    )
    causal_core = (
        acceptance["coverage"]
        and acceptance["restore_exact"]
        and acceptance["trajectory_shift"]
        and acceptance["eligibility_effect"]
    )
    searches = [
        value for row in shadow_rows for value in row["searches"]
    ] + [value for fork in forks for value in fork["searches"]]
    acceptance["search_p95"] = exp27.percentile(searches, 0.95) < 30.0
    return {
        "status": "pass" if causal_core and acceptance["search_p95"] else "fail",
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
            "tasks": task_ids,
            "episode_length": args.episode_length,
            "fork_horizon": args.fork_horizon,
            "regions": regions,
            "memories": len(memory["memories"]),
        },
        "shadow": {
            "steps": sum(row["steps"] for row in shadow_rows),
            "successes": sum(bool(row["success"]) for row in shadow_rows),
            "targets": len(targets),
        },
        "summary": summary,
        "search": exp27.latency_summary(searches),
        "acceptance": acceptance,
    }


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Physical AI VLA Trajectory Fork 029 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Status: {result['status']}",
        "",
    ]
    if result.get("stop_reason"):
        lines += [
            "## Result",
            "",
            f"Stopped at `{result['stop_reason']}` with "
            f"{result['target_count']}/{result['required']} required fork targets.",
        ]
        return "\n".join(lines) + "\n"
    overall = result["summary"]["overall"]
    correlation = overall["action_state_correlation"]
    correlation_text = "n/a" if correlation is None else f"{correlation:.3f}"
    lines += [
        "## Causal Result",
        "",
        f"Paired counterfactual forks: {overall['forks']} across "
        f"{overall['tasks']} tasks and {overall['phases']} trajectory phases.",
        "",
        "| Metric | Result |",
        "| --- | ---: |",
        f"| Historical-to-VLA action MAE | {overall['mean_action_mae']:.4f} |",
        f"| Post-action normalized state drift p50 | {overall['state_mae_p50']:.4f} |",
        f"| Post-action front-pixel MAE p50 | {overall['pixel_mae_p50']:.5f} |",
        f"| Branch minus control confidence | {overall['mean_confidence_delta']:.4f} |",
        f"| Control eligibility | {overall['control_eligibility']:.1%} |",
        f"| Historical branch eligibility | {overall['branch_eligibility']:.1%} |",
        f"| Eligibility drop | {overall['eligibility_drop']:.1%} |",
        f"| Action-error/state-drift correlation | {correlation_text} |",
        "",
        "## By Region",
        "",
        "| Region | Forks | Action MAE | State drift | Pixel MAE | Confidence delta | Control eligible | Branch eligible |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in result["summary"]["regions"]:
        lines.append(
            f"| {row['r']} | {row['n']} | {row['a']:.4f} | {row['s']:.4f} | "
            f"{row['p']:.5f} | {row['cd']:.4f} | {row['ce']:.1%} | "
            f"{row['be']:.1%} |"
        )
    lines += [
        "",
        "## Acceptance",
        "",
        "| Check | Status |",
        "| --- | --- |",
    ]
    lines.extend(
        f"| {name.replace('_', ' ')} | {'pass' if passed else 'fail'} |"
        for name, passed in result["acceptance"].items()
    )
    lines += [
        "",
        f"ZeptoDB search p50/p95: {result['search']['p50_ms']:.3f}/"
        f"{result['search']['p95_ms']:.3f} ms.",
        "",
        "## Interpretation",
        "",
        "The fork begins from an identical restored MuJoCo state. One branch uses",
        "the direct VLA action and the other uses one retrieved historical action;",
        "both then use deterministically paired VLA actions for up to four steps.",
        "This isolates short-horizon trajectory shift but is not physical-robot or",
        "multi-seed production evidence.",
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
    parser.add_argument("--task-ids", default="0,5")
    parser.add_argument(
        "--regions",
        default="broad:0.68:0,middle:0.72:0.005,selected:0.76:0.01",
    )
    parser.add_argument("--episode-length", type=int, default=520)
    parser.add_argument("--fork-horizon", type=int, default=4)
    parser.add_argument("--seed", type=int, default=28)
    parser.add_argument("--policy-fork-seed", type=int, default=290000)
    parser.add_argument("--required-streak", type=int, default=2)
    parser.add_argument("--min-forks", type=int, default=6)
    parser.add_argument("--min-state-divergence", type=float, default=0.01)
    parser.add_argument("--min-eligibility-drop", type=float, default=0.10)
    parser.add_argument("--min-confidence-drop", type=float, default=0.01)
    parser.add_argument("--min-correlation", type=float, default=0.30)
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
    if args.fork_horizon <= 0:
        parser.error("--fork-horizon must be positive")
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

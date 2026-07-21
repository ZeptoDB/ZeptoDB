#!/usr/bin/env python3
"""Run research-only Experiment 030 with separate confidence and safety gates."""

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


EXPERIMENT = 30
SUITE = exp27.SUITE
VLA_MODEL = exp27.VLA_MODEL
VLA_MODEL_SHA = exp27.VLA_MODEL_SHA
SIGLIP_MODEL = exp27.SIGLIP_MODEL


def derive_gripper_boundary(memories: list[dict[str, Any]]) -> float:
    if not memories:
        raise ValueError("memories must not be empty")
    widths_by_command: dict[int, list[float]] = {-1: [], 1: []}
    for row in memories:
        state = row.get("state")
        action = row.get("action")
        if not isinstance(state, list) or len(state) != 8:
            raise ValueError("memory state must have eight dimensions")
        if not isinstance(action, list) or len(action) != 7:
            raise ValueError("memory action must have seven dimensions")
        command = -1 if float(action[6]) < 0.0 else 1
        widths_by_command[command].append(float(state[6]) - float(state[7]))
    if not all(widths_by_command.values()):
        raise ValueError("memories must contain both gripper commands")
    open_width = statistics.fmean(widths_by_command[-1])
    closed_width = statistics.fmean(widths_by_command[1])
    return (open_width + closed_width) / 2.0


def candidate_disagreement(
    matches: list[dict[str, Any]],
    prior: list[float],
    action_scale: list[float],
) -> float:
    if not matches:
        raise ValueError("matches must not be empty")
    return statistics.fmean(
        exp26.normalized_action_mae(
            list(map(float, match["action"])), prior, action_scale
        )
        for match in matches
    )


def safety_index(
    *,
    action: list[float],
    state: list[float],
    state_means: list[float],
    state_stds: list[float],
    disagreement: float,
    gripper_consensus: bool,
    robot_contacts: int,
    gripper_boundary: float,
    translation_limit: float,
    rotation_limit: float,
    disagreement_limit: float,
    state_z_limit: float,
) -> dict[str, Any]:
    if len(action) != 7 or len(state) != 8:
        raise ValueError("action/state dimensions must be seven/eight")
    if len(state_means) != 8 or len(state_stds) != 8:
        raise ValueError("state statistics must have eight dimensions")
    limits = (
        translation_limit,
        rotation_limit,
        disagreement_limit,
        state_z_limit,
    )
    if any(value <= 0.0 for value in limits) or robot_contacts < 0:
        raise ValueError("safety limits must be positive and contacts non-negative")
    translation = math.sqrt(sum(float(value) ** 2 for value in action[:3]))
    rotation = math.sqrt(sum(float(value) ** 2 for value in action[3:6]))
    max_state_z = max(
        abs((float(value) - mean) / std)
        for value, mean, std in zip(state, state_means, state_stds)
    )
    width = float(state[6]) - float(state[7])
    expected_command = -1 if width >= gripper_boundary else 1
    candidate_command = -1 if float(action[6]) < 0.0 else 1
    gripper_transition = candidate_command != expected_command
    components = {
        "contact": 1.0 if robot_contacts else 0.0,
        "gripper_transition": 1.0 if gripper_transition else 0.0,
        "gripper_disagreement": 0.0 if gripper_consensus else 1.0,
        "translation": min(1.0, translation / translation_limit),
        "rotation": min(1.0, rotation / rotation_limit),
        "neighbor_disagreement": min(1.0, disagreement / disagreement_limit),
        "state_outlier": min(1.0, max_state_z / state_z_limit),
    }
    hard_risk = robot_contacts > 0 or gripper_transition or not gripper_consensus
    soft_risk = (
        translation > translation_limit
        or rotation > rotation_limit
        or disagreement > disagreement_limit
        or max_state_z > state_z_limit
    )
    level = "high" if hard_risk else "medium" if soft_risk else "low"
    return {
        "level": level,
        "score": max(components.values()),
        "components": components,
        "translation": translation,
        "rotation": rotation,
        "disagreement": disagreement,
        "max_state_z": max_state_z,
        "gripper_transition": gripper_transition,
    }


def route_dual_gate(
    *,
    confidence: float,
    margin: float,
    safety_level: str,
    previous_skip: bool,
    threshold: float,
    min_margin: float,
) -> tuple[bool, str]:
    if safety_level not in {"low", "medium", "high"}:
        raise ValueError("unknown safety level")
    if confidence < threshold or margin < min_margin:
        return False, "confidence"
    if safety_level != "low":
        return False, "safety"
    if previous_skip:
        return False, "cooldown"
    return True, "accepted"


def policy_seed(seed_base: int, task_id: int, step: int) -> int:
    if task_id < 0 or step < 0:
        raise ValueError("task and step must be non-negative")
    return seed_base + task_id * 10000 + step


def _robot_contact_count(env: Any) -> int:
    sim = env.envs[0]._env.sim
    robot_tokens = ("robot0", "gripper", "finger", "hand", "wrist")
    count = 0
    for index in range(int(sim.data.ncon)):
        contact = sim.data.contact[index]
        first = sim.model.geom_id2name(int(contact.geom1)) or ""
        second = sim.model.geom_id2name(int(contact.geom2)) or ""
        first_robot = any(token in first.lower() for token in robot_tokens)
        second_robot = any(token in second.lower() for token in robot_tokens)
        if first_robot != second_robot:
            count += 1
    return count


def _query_candidate(
    *,
    observation: dict[str, Any],
    task_index: int,
    args: argparse.Namespace,
    siglip: Any,
    siglip_processor: Any,
    client: exp27.AgentMemoryClient,
    memory: dict[str, Any],
    text_by_task: dict[int, Any],
) -> dict[str, Any]:
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
    matches, search_wall = exp28._search(
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
    gripper_signs = {-1 if float(match["action"][6]) < 0.0 else 1 for match in matches}
    return {
        "prior": prior,
        "confidence": confidence,
        "margin": margin,
        "disagreement": candidate_disagreement(
            matches, prior, memory["action_scale"]
        ),
        "gripper_consensus": len(gripper_signs) == 1,
        "encoder_wall": encoder_wall,
        "encoder_gpu": encoder_gpu,
        "search_wall": search_wall,
    }


def _safety_for_observation(
    *,
    env: Any,
    observation: dict[str, Any],
    candidate: dict[str, Any],
    memory: dict[str, Any],
    args: argparse.Namespace,
) -> dict[str, Any]:
    return safety_index(
        action=candidate["prior"],
        state=observation["observation.state"][0].tolist(),
        state_means=memory["state_means"],
        state_stds=memory["state_stds"],
        disagreement=float(candidate["disagreement"]),
        gripper_consensus=bool(candidate["gripper_consensus"]),
        robot_contacts=_robot_contact_count(env),
        gripper_boundary=float(memory["gripper_boundary"]),
        translation_limit=args.translation_limit,
        rotation_limit=args.rotation_limit,
        disagreement_limit=args.disagreement_limit,
        state_z_limit=args.state_z_limit,
    )


def _policy_action_for_step(
    *,
    task_id: int,
    step: int,
    args: argparse.Namespace,
    policy: Any,
    preprocessor: Any,
    postprocessor: Any,
    env_postprocessor: Any,
    observation: dict[str, Any],
) -> tuple[Any, float, float]:
    exp27.seed_policy_rng(policy_seed(args.policy_seed_base, task_id, step))
    policy.reset()
    return exp27._policy_action(
        policy,
        preprocessor,
        postprocessor,
        env_postprocessor,
        observation,
    )


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
) -> dict[str, Any]:
    import numpy as np
    from lerobot.envs.factory import make_env_pre_post_processors

    env, env_config = exp27._make_task_env(task_id, args.episode_length)
    env_preprocessor, env_postprocessor = make_env_pre_post_processors(
        env_config, policy.config
    )
    raw_observation, _ = env.reset(seed=[seed])
    task_description = list(env.call("task_description"))[0]
    task_index = exp27.resolve_memory_task_index(
        task_description, memory["text_to_task"]
    )
    decision_wall: list[float] = []
    policy_wall: list[float] = []
    policy_gpu: list[float] = []
    encoder_gpu: list[float] = []
    search_wall: list[float] = []
    accepted_errors: list[float] = []
    risk_counts = {"low": 0, "medium": 0, "high": 0}
    fallback_counts = {"confidence": 0, "safety": 0, "cooldown": 0}
    skips = 0
    vla_calls = 0
    unsafe_adoptions = 0
    post_skip_hazards = 0
    previous_skip = False
    check_post_skip = False
    success = False
    reward_total = 0.0
    steps = 0
    try:
        for step in range(args.episode_length):
            observation = exp27._processed_observation(
                env, raw_observation, env_preprocessor
            )
            decision_start = time.perf_counter_ns()
            candidate = _query_candidate(
                observation=observation,
                task_index=task_index,
                args=args,
                siglip=siglip,
                siglip_processor=siglip_processor,
                client=client,
                memory=memory,
                text_by_task=text_by_task,
            )
            safety = _safety_for_observation(
                env=env,
                observation=observation,
                candidate=candidate,
                memory=memory,
                args=args,
            )
            risk_counts[str(safety["level"])] += 1
            if check_post_skip and (
                safety["components"]["contact"] > 0.0
                or float(safety["max_state_z"]) > args.hazard_state_z
            ):
                post_skip_hazards += 1
            check_post_skip = False
            should_skip, reason = route_dual_gate(
                confidence=float(candidate["confidence"]),
                margin=float(candidate["margin"]),
                safety_level=str(safety["level"]),
                previous_skip=previous_skip,
                threshold=args.threshold,
                min_margin=args.min_margin,
            )
            if variant == "shadow":
                action, vla_wall, vla_gpu = _policy_action_for_step(
                    task_id=task_id,
                    step=step,
                    args=args,
                    policy=policy,
                    preprocessor=preprocessor,
                    postprocessor=postprocessor,
                    env_postprocessor=env_postprocessor,
                    observation=observation,
                )
                vla_calls += 1
                policy_wall.append(vla_wall)
                policy_gpu.append(vla_gpu)
                if should_skip:
                    accepted_errors.append(
                        exp26.normalized_action_mae(
                            exp27._prior_action(
                                candidate["prior"], env_postprocessor
                            ).reshape(-1).tolist(),
                            action.reshape(-1).tolist(),
                            memory["action_scale"],
                        )
                    )
                    skips += 1
                previous_skip = should_skip
            elif variant == "routed":
                if should_skip:
                    action = exp27._prior_action(
                        candidate["prior"], env_postprocessor
                    )
                    skips += 1
                    check_post_skip = True
                    if safety["level"] != "low":
                        unsafe_adoptions += 1
                else:
                    action, vla_wall, vla_gpu = _policy_action_for_step(
                        task_id=task_id,
                        step=step,
                        args=args,
                        policy=policy,
                        preprocessor=preprocessor,
                        postprocessor=postprocessor,
                        env_postprocessor=env_postprocessor,
                        observation=observation,
                    )
                    vla_calls += 1
                    policy_wall.append(vla_wall)
                    policy_gpu.append(vla_gpu)
                    fallback_counts[reason] += 1
                previous_skip = should_skip
            else:
                raise ValueError(f"unknown variant: {variant}")
            encoder_gpu.append(float(candidate["encoder_gpu"]))
            search_wall.append(float(candidate["search_wall"]))
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
        "decision_wall": decision_wall,
        "policy_wall": policy_wall,
        "policy_gpu": policy_gpu,
        "encoder_gpu": encoder_gpu,
        "search_wall": search_wall,
        "accepted_errors": accepted_errors,
        "risk_counts": risk_counts,
        "fallback_counts": fallback_counts,
        "unsafe_adoptions": unsafe_adoptions,
        "post_skip_hazards": post_skip_hazards,
    }


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    task_ids = exp28.parse_task_ids(args.task_ids)
    siglip, siglip_processor, policy, preprocessor, postprocessor = exp26._load_models(
        args
    )
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    if int(policy.config.n_action_steps) != 1:
        raise RuntimeError("Experiment 030 requires n_action_steps=1")
    client = exp27.AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    memory, text_by_task = exp28._prepare_memory_bank(
        args, siglip, siglip_processor, client
    )
    memory["gripper_boundary"] = derive_gripper_boundary(memory["memories"])

    def run(variant: str, task_id: int) -> dict[str, Any]:
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
        )

    shadow_rows = [run("shadow", task_id) for task_id in task_ids]
    shadow_decisions = sum(row["steps"] for row in shadow_rows)
    shadow_eligible = sum(row["skips"] for row in shadow_rows)
    shadow_rate = shadow_eligible / shadow_decisions
    accepted_errors = [
        value for row in shadow_rows for value in row["accepted_errors"]
    ]
    preflight = {
        "nontrivial_low_risk_slice": shadow_rate >= args.min_shadow_skip_rate,
        "accepted_action_error": (
            bool(accepted_errors)
            and exp27.percentile(accepted_errors, 0.95)
            <= args.max_accepted_action_mae_p95
        ),
    }
    if not all(preflight.values()):
        searches = [
            value for row in shadow_rows for value in row["search_wall"]
        ]
        return {
            "status": "fail",
            "experiment": EXPERIMENT,
            "stop_reason": "shadow_safety_preflight_failed",
            "scope": {"tasks": task_ids, "steps": shadow_decisions},
            "shadow": {
                "eligible": shadow_eligible,
                "skip_rate": shadow_rate,
                "action_mae_p95": exp27.percentile(accepted_errors, 0.95),
                "risk": {
                    level: sum(row["risk_counts"][level] for row in shadow_rows)
                    for level in ("low", "medium", "high")
                },
                "search_p95": exp27.percentile(searches, 0.95),
            },
            "acceptance": preflight,
        }

    routed_rows = [run("routed", task_id) for task_id in task_ids]
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
        for task_id, shadow, routed in zip(task_ids, shadow_rows, routed_rows)
    ]
    quality = exp27.paired_quality(task_results)
    routed_decisions = sum(row["steps"] for row in routed_rows)
    routed_skips = sum(row["skips"] for row in routed_rows)
    skip_rate = routed_skips / routed_decisions
    baseline_policy = [
        value for row in shadow_rows for value in row["policy_wall"]
    ]
    routed_decision = [
        value for row in routed_rows for value in row["decision_wall"]
    ]
    latency_reduction = 1.0 - statistics.fmean(
        routed_decision
    ) / statistics.fmean(baseline_policy)
    baseline_gpu = _mean(
        [value for row in shadow_rows for value in row["policy_gpu"]]
    )
    routed_gpu = (
        sum(value for row in routed_rows for value in row["policy_gpu"])
        + sum(value for row in routed_rows for value in row["encoder_gpu"])
    ) / routed_decisions
    searches = [value for row in routed_rows for value in row["search_wall"]]
    unsafe_adoptions = sum(row["unsafe_adoptions"] for row in routed_rows)
    post_skip_hazards = sum(row["post_skip_hazards"] for row in routed_rows)
    acceptance = {
        **preflight,
        "success_noninferior": quality["success_rate_delta"] >= 0.0,
        "paired_regressions": quality["paired_regressions"] == 0,
        "actual_skip_rate": skip_rate >= args.min_actual_skip_rate,
        "latency_reduction": latency_reduction >= args.min_latency_reduction,
        "unsafe_adoptions": unsafe_adoptions == 0,
        "post_skip_hazards": post_skip_hazards == 0,
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
        "scope": {
            "tasks": task_ids,
            "episode_length": args.episode_length,
            "threshold": args.threshold,
            "min_margin": args.min_margin,
            "max_consecutive_skips": 1,
            "gripper_boundary": memory["gripper_boundary"],
        },
        "shadow": {
            "steps": shadow_decisions,
            "eligible": shadow_eligible,
            "skip_rate": shadow_rate,
            "action_mae_mean": _mean(accepted_errors),
            "action_mae_p95": exp27.percentile(accepted_errors, 0.95),
            "risk": {
                level: sum(row["risk_counts"][level] for row in shadow_rows)
                for level in ("low", "medium", "high")
            },
        },
        "quality": quality,
        "baseline": {
            "steps": sum(row["steps"] for row in shadow_rows),
            "vla_calls": sum(row["vla_calls"] for row in shadow_rows),
            "mean_decision_ms": statistics.fmean(baseline_policy),
            "gpu_per_decision_ms": baseline_gpu,
        },
        "routed": {
            "steps": routed_decisions,
            "vla_calls": sum(row["vla_calls"] for row in routed_rows),
            "skips": routed_skips,
            "skip_rate": skip_rate,
            "decision": exp27.latency_summary(routed_decision),
            "latency_reduction": latency_reduction,
            "gpu_per_decision_ms": routed_gpu,
            "gpu_reduction": 1.0 - routed_gpu / baseline_gpu,
            "unsafe_adoptions": unsafe_adoptions,
            "post_skip_hazards": post_skip_hazards,
            "search": exp27.latency_summary(searches),
            "fallback": {
                reason: sum(row["fallback_counts"][reason] for row in routed_rows)
                for reason in ("confidence", "safety", "cooldown")
            },
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


def _mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Physical AI VLA Confidence-Safety Gate 030 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Status: {result['status']}",
        "",
    ]
    if result.get("stop_reason"):
        shadow = result["shadow"]
        lines += [
            "## Result",
            "",
            f"Stopped at `{result['stop_reason']}` before routed execution.",
            f"Shadow low-risk eligible rate: {shadow['skip_rate']:.1%}.",
            f"Accepted candidate action MAE p95: {shadow['action_mae_p95']:.4f}.",
            f"ZeptoDB search p95: {shadow['search_p95']:.3f} ms.",
            "",
            f"Risk counts: low {shadow['risk']['low']}, medium "
            f"{shadow['risk']['medium']}, high {shadow['risk']['high']}.",
        ]
        return "\n".join(lines) + "\n"
    quality = result["quality"]
    baseline = result["baseline"]
    routed = result["routed"]
    shadow = result["shadow"]
    lines += [
        "## Shadow Safety Slice",
        "",
        f"Low-risk, high-confidence eligibility: {shadow['eligible']}/"
        f"{shadow['steps']} ({shadow['skip_rate']:.1%}).",
        f"Accepted action MAE mean/p95: {shadow['action_mae_mean']:.4f}/"
        f"{shadow['action_mae_p95']:.4f}.",
        f"Risk counts: low {shadow['risk']['low']}, medium "
        f"{shadow['risk']['medium']}, high {shadow['risk']['high']}.",
        "",
        "## Closed-Loop Result",
        "",
        "| Path | Success | Steps | VLA calls | Skips | Mean decision ms | GPU ms/decision |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        f"| Direct | {quality['baseline_successes']}/{quality['tasks']} | "
        f"{baseline['steps']} | {baseline['vla_calls']} | 0 | "
        f"{baseline['mean_decision_ms']:.3f} | "
        f"{baseline['gpu_per_decision_ms']:.3f} |",
        f"| Dual gate | {quality['routed_successes']}/{quality['tasks']} | "
        f"{routed['steps']} | {routed['vla_calls']} | {routed['skips']} | "
        f"{routed['decision']['mean_ms']:.3f} | "
        f"{routed['gpu_per_decision_ms']:.3f} |",
        "",
        f"- Actual skip rate: {routed['skip_rate']:.1%}.",
        f"- Corrected latency reduction: {routed['latency_reduction']:.1%}.",
        f"- GPU-time reduction per decision: {routed['gpu_reduction']:.1%}.",
        f"- Paired regressions: {quality['paired_regressions']}.",
        f"- Unsafe historical adoptions: {routed['unsafe_adoptions']}.",
        f"- Post-skip proxy hazards: {routed['post_skip_hazards']}.",
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
    lines += ["", "## Acceptance", "", "| Check | Status |", "| --- | --- |"]
    lines.extend(
        f"| {name.replace('_', ' ')} | {'pass' if passed else 'fail'} |"
        for name, passed in result["acceptance"].items()
    )
    lines += [
        "",
        "## Interpretation",
        "",
        "Confidence controls whether memory reuse is plausible. The independent",
        "safety gate uses only state, contact, retrieved-action, and neighbor",
        "agreement signals, so rejecting an action does not require a VLA call.",
        "This bounded simulator result is not a physical-robot safety claim.",
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
    parser.add_argument("--episode-length", type=int, default=520)
    parser.add_argument("--seed", type=int, default=28)
    parser.add_argument("--policy-seed-base", type=int, default=300000)
    parser.add_argument("--threshold", type=float, default=0.76)
    parser.add_argument("--min-margin", type=float, default=0.01)
    parser.add_argument("--translation-limit", type=float, default=0.75)
    parser.add_argument("--rotation-limit", type=float, default=0.15)
    parser.add_argument("--disagreement-limit", type=float, default=0.30)
    parser.add_argument("--state-z-limit", type=float, default=3.0)
    parser.add_argument("--hazard-state-z", type=float, default=4.0)
    parser.add_argument("--min-shadow-skip-rate", type=float, default=0.20)
    parser.add_argument("--max-accepted-action-mae-p95", type=float, default=0.50)
    parser.add_argument("--min-actual-skip-rate", type=float, default=0.20)
    parser.add_argument("--min-latency-reduction", type=float, default=0.15)
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

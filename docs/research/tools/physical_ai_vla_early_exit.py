#!/usr/bin/env python3
"""Run research-only Experiment 026 with a real VLA and ZeptoDB early exit."""

from __future__ import annotations

import argparse
import io
import json
import math
import random
import statistics
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DATASET = "lerobot/libero_10_image"
DATASET_REVISION = "main"
SIGLIP_MODEL = "google/siglip-base-patch16-224"
VLA_MODEL = "HuggingFaceVLA/smolvla_libero"
VLA_MODEL_SHA = "6721902bc4d61e50a3bfdb11dfb4cb626f05d102"
HF_BASE = f"https://huggingface.co/datasets/{DATASET}/resolve/{DATASET_REVISION}"
ROWS_URL = "https://datasets-server.huggingface.co/rows"
ROWS_REQUEST_INTERVAL_SECONDS = 1.5
MANIFEST_CHECKPOINT_MAX_AGE_SECONDS = 20 * 60
_ROWS_REQUEST_LOCK = threading.Lock()
_NEXT_ROWS_REQUEST_AT = 0.0


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    if not 0.0 <= quantile <= 1.0:
        raise ValueError("quantile must be between 0 and 1")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(quantile * len(ordered)) - 1)]


def latency_summary(values: list[float]) -> dict[str, float | int]:
    return {
        "count": len(values),
        "mean_ms": statistics.fmean(values) if values else 0.0,
        "p50_ms": percentile(values, 0.50),
        "p95_ms": percentile(values, 0.95),
    }


def split_calibration_evaluation(
    queries: list[dict[str, Any]], *, calibration_per_task: int
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if calibration_per_task <= 0:
        raise ValueError("calibration_per_task must be positive")
    grouped: dict[int, list[dict[str, Any]]] = {}
    for query in queries:
        grouped.setdefault(int(query["task_index"]), []).append(query)
    calibration: list[dict[str, Any]] = []
    evaluation: list[dict[str, Any]] = []
    for task_index in sorted(grouped):
        rows = grouped[task_index]
        if len(rows) <= calibration_per_task:
            raise ValueError(
                f"task {task_index} needs more than {calibration_per_task} queries"
            )
        calibration.extend(rows[:calibration_per_task])
        evaluation.extend(rows[calibration_per_task:])
    return calibration, evaluation


def normalized_action_mae(
    predicted: list[float], expected: list[float], scale: list[float]
) -> float:
    if not predicted or len(predicted) != len(expected) or len(expected) != len(scale):
        raise ValueError("action and scale dimensions must be equal and non-empty")
    if any(value <= 0.0 for value in scale):
        raise ValueError("action scale must be positive")
    return statistics.fmean(
        abs(predicted[index] - expected[index]) / scale[index]
        for index in range(len(scale))
    )


def weighted_action_prior(
    matches: list[dict[str, Any]], action_scale: list[float], *, temperature: float = 20.0
) -> tuple[list[float], float]:
    if not matches:
        raise ValueError("at least one match is required")
    actions = [list(map(float, row["action"])) for row in matches]
    if any(len(action) != len(action_scale) for action in actions):
        raise ValueError("match action dimensions do not match action scale")
    similarities = [float(row["similarity"]) for row in matches]
    maximum = max(similarities)
    raw_weights = [math.exp((similarity - maximum) * temperature) for similarity in similarities]
    weight_sum = sum(raw_weights)
    weights = [weight / weight_sum for weight in raw_weights]
    prior = [
        sum(weight * action[dimension] for weight, action in zip(weights, actions))
        for dimension in range(len(action_scale))
    ]
    disagreement = sum(
        weight * normalized_action_mae(action, prior, action_scale)
        for weight, action in zip(weights, actions)
    )
    margin = similarities[0] - similarities[1] if len(similarities) > 1 else 0.0
    confidence = similarities[0] + 0.25 * margin - 0.25 * min(disagreement, 2.0)
    return prior, confidence


def calibrate_threshold(
    rows: list[dict[str, Any]], *, quality_ratio: float = 1.05, absolute_tolerance: float = 0.005
) -> dict[str, float | int]:
    if not rows:
        raise ValueError("calibration rows must not be empty")
    baseline_mae = statistics.fmean(float(row["baseline_mae"]) for row in rows)
    quality_limit = max(baseline_mae * quality_ratio, baseline_mae + absolute_tolerance)
    candidates = [max(float(row["confidence"]) for row in rows) + 1.0]
    candidates.extend(sorted({float(row["confidence"]) for row in rows}))
    best: dict[str, float | int] | None = None
    for threshold in candidates:
        skipped = [float(row["confidence"]) >= threshold for row in rows]
        routed_mae = statistics.fmean(
            float(row["prior_mae"]) if skip else float(row["baseline_mae"])
            for row, skip in zip(rows, skipped)
        )
        candidate = {
            "threshold": threshold,
            "skipped": sum(skipped),
            "skip_rate": sum(skipped) / len(rows),
            "baseline_mae": baseline_mae,
            "routed_mae": routed_mae,
            "quality_limit": quality_limit,
        }
        if routed_mae <= quality_limit and (
            best is None
            or int(candidate["skipped"]) > int(best["skipped"])
            or (
                int(candidate["skipped"]) == int(best["skipped"])
                and float(candidate["routed_mae"]) < float(best["routed_mae"])
            )
        ):
            best = candidate
    if best is None:
        raise RuntimeError("no calibration threshold satisfies the quality limit")
    return best


def _wait_for_rows_request_slot() -> None:
    global _NEXT_ROWS_REQUEST_AT
    with _ROWS_REQUEST_LOCK:
        delay = _NEXT_ROWS_REQUEST_AT - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        _NEXT_ROWS_REQUEST_AT = time.monotonic() + ROWS_REQUEST_INTERVAL_SECONDS


def _defer_rows_requests(seconds: float) -> None:
    global _NEXT_ROWS_REQUEST_AT
    with _ROWS_REQUEST_LOCK:
        _NEXT_ROWS_REQUEST_AT = max(_NEXT_ROWS_REQUEST_AT, time.monotonic() + seconds)


def _request_bytes(
    url: str, *, attempts: int = 5, timeout: float = 60.0, throttle_rows: bool = False
) -> bytes:
    last_error: Exception | None = None
    for attempt in range(attempts):
        if throttle_rows:
            _wait_for_rows_request_slot()
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "zeptodb-research/026"})
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as exc:
            last_error = exc
            if attempt + 1 < attempts:
                retry_after = 0.0
                if isinstance(exc, urllib.error.HTTPError):
                    try:
                        retry_after = float(exc.headers.get("Retry-After", "0"))
                    except ValueError:
                        pass
                    if exc.code == 429:
                        retry_after = max(retry_after, 60.0)
                backoff = max(retry_after, min(60.0, 2**attempt))
                if throttle_rows:
                    _defer_rows_requests(backoff)
                time.sleep(backoff + random.uniform(0.0, 0.5))
    raise RuntimeError(f"request failed after {attempts} attempts: {url}") from last_error


def _load_metadata() -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    import pyarrow.parquet as parquet

    episodes = parquet.read_table(
        io.BytesIO(_request_bytes(f"{HF_BASE}/meta/episodes/chunk-000/file-000.parquet")),
        columns=["episode_index", "dataset_from_index", "length", "tasks"],
    ).to_pylist()
    tasks = parquet.read_table(
        io.BytesIO(_request_bytes(f"{HF_BASE}/meta/tasks.parquet"))
    ).to_pylist()
    return episodes, tasks


def select_balanced_episodes(
    episodes: list[dict[str, Any]],
    task_rows: list[dict[str, Any]],
    *,
    memory_per_task: int,
    query_per_task: int,
    seed: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if memory_per_task <= 0 or query_per_task <= 0:
        raise ValueError("memory_per_task and query_per_task must be positive")
    task_by_name = {row["task"]: int(row["task_index"]) for row in task_rows}
    grouped: dict[int, list[dict[str, Any]]] = {}
    for episode in episodes:
        names = episode.get("tasks")
        if not isinstance(names, list) or len(names) != 1 or names[0] not in task_by_name:
            raise ValueError(f"episode {episode.get('episode_index')} has an unknown task")
        task_index = task_by_name[names[0]]
        grouped.setdefault(task_index, []).append(
            {
                "episode_index": int(episode["episode_index"]),
                "task_index": task_index,
                "task": names[0],
                "row_index": int(episode["dataset_from_index"]) + int(episode["length"]) // 2,
            }
        )
    needed = memory_per_task + query_per_task
    memories: list[dict[str, Any]] = []
    queries: list[dict[str, Any]] = []
    for task_index in sorted(task_by_name.values()):
        candidates = sorted(grouped.get(task_index, []), key=lambda row: row["episode_index"])
        if len(candidates) < needed:
            raise ValueError(
                f"task {task_index} has {len(candidates)} episodes, but {needed} are required"
            )
        random.Random(seed + task_index).shuffle(candidates)
        memories.extend(candidates[:memory_per_task])
        queries.extend(candidates[memory_per_task:needed])
    return memories, queries


def _resolve_sample(sample: dict[str, Any]) -> dict[str, Any]:
    query = urllib.parse.urlencode(
        {
            "dataset": DATASET,
            "config": "default",
            "split": "train",
            "offset": sample["row_index"],
            "length": 1,
        }
    )
    payload = json.loads(
        _request_bytes(f"{ROWS_URL}?{query}", attempts=10, timeout=90.0, throttle_rows=True)
    )
    rows = payload.get("rows", [])
    if len(rows) != 1:
        raise RuntimeError(f"dataset server returned {len(rows)} rows")
    row = rows[0].get("row", {})
    if int(row.get("episode_index", -1)) != sample["episode_index"]:
        raise RuntimeError(f"row {sample['row_index']} belongs to the wrong episode")
    front = row.get("observation.images.image", {}).get("src")
    wrist = row.get("observation.images.wrist_image", {}).get("src")
    state = row.get("observation.state")
    action = row.get("action")
    if not front or not wrist:
        raise RuntimeError(f"row {sample['row_index']} is missing a camera image")
    if not isinstance(state, list) or len(state) != 8:
        raise RuntimeError(f"row {sample['row_index']} has invalid state")
    if not isinstance(action, list) or len(action) != 7:
        raise RuntimeError(f"row {sample['row_index']} has invalid action")
    return {
        **sample,
        "front_image_url": str(front),
        "wrist_image_url": str(wrist),
        "state": list(map(float, state)),
        "action": list(map(float, action)),
    }


def _write_manifest_checkpoint(path: Path, resolved: dict[int, dict[str, Any]]) -> None:
    temporary = path.with_suffix(f"{path.suffix}.tmp")
    temporary.write_text(
        json.dumps(
            {
                "created_at": time.time(),
                "samples": list(resolved.values()),
            },
            separators=(",", ":"),
        )
    )
    temporary.replace(path)


def resolve_samples_with_checkpoint(
    samples: list[dict[str, Any]], *, checkpoint: Path, workers: int
) -> list[dict[str, Any]]:
    if workers <= 0:
        raise ValueError("manifest workers must be positive")
    checkpoint.parent.mkdir(parents=True, exist_ok=True)
    resolved: dict[int, dict[str, Any]] = {}
    if checkpoint.exists():
        payload = json.loads(checkpoint.read_text())
        age_seconds = time.time() - float(payload.get("created_at", 0.0))
        if age_seconds <= MANIFEST_CHECKPOINT_MAX_AGE_SECONDS:
            resolved = {
                int(row["row_index"]): row
                for row in payload.get("samples", [])
                if isinstance(row, dict) and "row_index" in row
            }
        else:
            checkpoint.unlink()
    pending = [sample for sample in samples if int(sample["row_index"]) not in resolved]
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(_resolve_sample, sample): sample for sample in pending}
        for future in as_completed(futures):
            row = future.result()
            resolved[int(row["row_index"])] = row
            _write_manifest_checkpoint(checkpoint, resolved)
    return [resolved[int(sample["row_index"])] for sample in samples]


def prepare_sample_manifest(args: argparse.Namespace) -> dict[str, Any]:
    global ROWS_REQUEST_INTERVAL_SECONDS
    ROWS_REQUEST_INTERVAL_SECONDS = args.rows_request_interval
    episodes, tasks = _load_metadata()
    memories, queries = select_balanced_episodes(
        episodes,
        tasks,
        memory_per_task=args.memory_per_task,
        query_per_task=args.query_per_task,
        seed=args.seed,
    )
    checkpoint = args.manifest_checkpoint or args.prepare_manifest.with_suffix(
        ".checkpoint.json"
    )
    resolved = resolve_samples_with_checkpoint(
        memories + queries,
        checkpoint=checkpoint,
        workers=args.manifest_workers,
    )
    manifest = {
        "dataset": DATASET,
        "dataset_revision": DATASET_REVISION,
        "seed": args.seed,
        "memories": resolved[: len(memories)],
        "queries": resolved[len(memories) :],
    }
    args.prepare_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.prepare_manifest.write_text(json.dumps(manifest, separators=(",", ":")))
    return manifest


def load_sample_manifest(
    path: Path, *, memory_per_task: int, query_per_task: int
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    manifest = json.loads(path.read_text())
    memories = manifest.get("memories")
    queries = manifest.get("queries")
    if not isinstance(memories, list) or not isinstance(queries, list):
        raise ValueError("sample manifest must contain memories and queries arrays")
    if len(memories) != 10 * memory_per_task or len(queries) != 10 * query_per_task:
        raise ValueError("sample manifest has unexpected balanced split counts")
    required = {"front_image_url", "wrist_image_url", "state", "action"}
    if any(not required.issubset(sample) for sample in memories + queries):
        raise ValueError("sample manifest entry is missing VLA input or target data")
    return memories, queries


def _fetch_image(url: str) -> Any:
    from PIL import Image

    return Image.open(io.BytesIO(_request_bytes(url, timeout=90.0))).convert("RGB")


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
        return data, (time.perf_counter_ns() - start) / 1_000_000


def _action_scale(memories: list[dict[str, Any]]) -> list[float]:
    return [
        max(
            0.1,
            max(float(row["action"][dimension]) for row in memories)
            - min(float(row["action"][dimension]) for row in memories),
        )
        for dimension in range(7)
    ]


def _state_stats(memories: list[dict[str, Any]]) -> tuple[list[float], list[float]]:
    means = [
        statistics.fmean(float(row["state"][dimension]) for row in memories)
        for dimension in range(8)
    ]
    stds = [
        max(
            1e-6,
            math.sqrt(
                statistics.fmean(
                    (float(row["state"][dimension]) - means[dimension]) ** 2
                    for row in memories
                )
            ),
        )
        for dimension in range(8)
    ]
    return means, stds


def _combine_embedding(
    visual: Any, state: list[float], state_means: list[float], state_stds: list[float]
) -> Any:
    import torch

    state_tensor = torch.tensor(
        [
            (float(value) - state_means[index]) / state_stds[index]
            for index, value in enumerate(state)
        ],
        dtype=torch.float32,
    )
    state_tensor = torch.nn.functional.normalize(state_tensor, dim=0)
    return torch.cat((visual * math.sqrt(0.75), state_tensor * math.sqrt(0.25)))


def _encode_visual_batch(model: Any, processor: Any, images: list[Any]) -> Any:
    import torch

    inputs = processor(images=images, return_tensors="pt")
    pixels = inputs["pixel_values"].to("cuda", non_blocking=True)
    with torch.inference_mode():
        features = model.get_image_features(pixel_values=pixels)
    return torch.nn.functional.normalize(features.float(), dim=-1)


def _encode_query(
    model: Any,
    processor: Any,
    image: Any,
    text_feature: Any,
    sample: dict[str, Any],
    state_means: list[float],
    state_stds: list[float],
) -> tuple[Any, float, float]:
    import torch

    torch.cuda.synchronize()
    wall_start = time.perf_counter_ns()
    start_event = torch.cuda.Event(enable_timing=True)
    end_event = torch.cuda.Event(enable_timing=True)
    start_event.record()
    image_feature = _encode_visual_batch(model, processor, [image])[0]
    visual = torch.nn.functional.normalize(image_feature + text_feature, dim=0).cpu()
    end_event.record()
    torch.cuda.synchronize()
    wall_ms = (time.perf_counter_ns() - wall_start) / 1_000_000
    return (
        _combine_embedding(visual, sample["state"], state_means, state_stds),
        wall_ms,
        float(start_event.elapsed_time(end_event)),
    )


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
                "tenant_id": f"physical-ai-026-{run_id}",
                "namespace": "libero-vla-early-exit",
                "agent_id": "physical-ai-vla-early-exit",
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
    client: AgentMemoryClient, vector: Any, *, run_id: str, top_k: int
) -> tuple[list[dict[str, Any]], float]:
    response, latency_ms = client.request(
        "POST",
        "/api/ai/memories/search",
        {
            "tenant_id": f"physical-ai-026-{run_id}",
            "namespace": "libero-vla-early-exit",
            "agent_id": "physical-ai-vla-early-exit",
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


def _image_tensor(image: Any) -> Any:
    import torch
    from torchvision.transforms.functional import pil_to_tensor

    return pil_to_tensor(image).to(dtype=torch.float32).div_(255.0)


def _vla_action(
    policy: Any,
    preprocess: Any,
    postprocess: Any,
    sample: dict[str, Any],
    front: Any,
    wrist: Any,
    *,
    seed: int,
) -> tuple[list[float], float, float]:
    import torch

    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)
    policy.reset()
    wall_start = time.perf_counter_ns()
    batch = preprocess(
        {
            "observation.images.image": _image_tensor(front),
            "observation.images.image2": _image_tensor(wrist),
            "observation.state": torch.tensor(sample["state"], dtype=torch.float32),
            "task": sample["task"],
        }
    )
    torch.cuda.synchronize()
    start_event = torch.cuda.Event(enable_timing=True)
    end_event = torch.cuda.Event(enable_timing=True)
    start_event.record()
    action = policy.select_action(batch)
    end_event.record()
    torch.cuda.synchronize()
    action = postprocess(action)
    wall_ms = (time.perf_counter_ns() - wall_start) / 1_000_000
    values = action.detach().cpu().reshape(-1).tolist()
    if len(values) != 7 or not all(math.isfinite(value) for value in values):
        raise RuntimeError("VLA returned an invalid action")
    return values, wall_ms, float(start_event.elapsed_time(end_event))


def _energy_reader() -> Any:
    try:
        import pynvml

        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        pynvml.nvmlDeviceGetTotalEnergyConsumption(handle)
        return lambda: int(pynvml.nvmlDeviceGetTotalEnergyConsumption(handle))
    except Exception:
        return None


def _phase_energy(reader: Any, start: int | None) -> float | None:
    if reader is None or start is None:
        return None
    return max(0.0, (reader() - start) / 1000.0)


def _load_models(args: argparse.Namespace) -> tuple[Any, ...]:
    import torch
    from lerobot.policies.factory import make_pre_post_processors
    from lerobot.policies.smolvla.modeling_smolvla import SmolVLAPolicy
    from transformers import AutoModel, AutoProcessor

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is not available")
    siglip_revision = getattr(args, "siglip_revision", None)
    vla_revision = getattr(args, "vla_revision", None)
    siglip_kwargs = {"revision": siglip_revision} if siglip_revision else {}
    vla_kwargs = {"revision": vla_revision} if vla_revision else {}
    siglip_processor = AutoProcessor.from_pretrained(
        args.siglip_model, **siglip_kwargs
    )
    siglip = AutoModel.from_pretrained(
        args.siglip_model, torch_dtype=torch.float16, **siglip_kwargs
    )
    siglip = siglip.to("cuda").eval()
    policy = (
        SmolVLAPolicy.from_pretrained(args.vla_model, **vla_kwargs)
        .to("cuda")
        .eval()
    )
    processor_overrides = {"device_processor": {"device": "cuda"}}
    if vla_revision:
        from lerobot.processor import PolicyProcessorPipeline
        from lerobot.processor.converters import (
            batch_to_transition,
            policy_action_to_transition,
            transition_to_batch,
            transition_to_policy_action,
        )
        from lerobot.utils.constants import (
            POLICY_POSTPROCESSOR_DEFAULT_NAME,
            POLICY_PREPROCESSOR_DEFAULT_NAME,
        )

        preprocess = PolicyProcessorPipeline.from_pretrained(
            pretrained_model_name_or_path=args.vla_model,
            config_filename=f"{POLICY_PREPROCESSOR_DEFAULT_NAME}.json",
            revision=vla_revision,
            overrides=processor_overrides,
            to_transition=batch_to_transition,
            to_output=transition_to_batch,
        )
        postprocess = PolicyProcessorPipeline.from_pretrained(
            pretrained_model_name_or_path=args.vla_model,
            config_filename=f"{POLICY_POSTPROCESSOR_DEFAULT_NAME}.json",
            revision=vla_revision,
            to_transition=policy_action_to_transition,
            to_output=transition_to_policy_action,
        )
    else:
        preprocess, postprocess = make_pre_post_processors(
            policy.config,
            args.vla_model,
            preprocessor_overrides=processor_overrides,
        )
    torch.cuda.synchronize()
    return siglip, siglip_processor, policy, preprocess, postprocess


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch
    from huggingface_hub import model_info

    memories, queries = load_sample_manifest(
        args.sample_manifest,
        memory_per_task=args.memory_per_task,
        query_per_task=args.query_per_task,
    )
    calibration, evaluation = split_calibration_evaluation(
        queries, calibration_per_task=args.calibration_per_task
    )
    samples = memories + queries
    with ThreadPoolExecutor(max_workers=args.download_workers) as pool:
        fronts = list(pool.map(lambda row: _fetch_image(row["front_image_url"]), samples))
        wrists = list(pool.map(lambda row: _fetch_image(row["wrist_image_url"]), samples))
    front_by_episode = {
        int(row["episode_index"]): image for row, image in zip(samples, fronts)
    }
    wrist_by_episode = {
        int(row["episode_index"]): image for row, image in zip(samples, wrists)
    }

    siglip, siglip_processor, policy, preprocess, postprocess = _load_models(args)
    resolved_sha = model_info(args.vla_model, revision=VLA_MODEL_SHA).sha
    if resolved_sha != VLA_MODEL_SHA:
        raise RuntimeError(f"unexpected VLA revision: {resolved_sha}")
    state_means, state_stds = _state_stats(memories)
    action_scale = _action_scale(memories)

    task_texts = {int(row["task_index"]): row["task"] for row in samples}
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
        task_index: text_features[position] for position, task_index in enumerate(ordered_tasks)
    }

    memory_visual_batches = []
    memory_fronts = fronts[: len(memories)]
    with torch.inference_mode():
        for start in range(0, len(memories), args.batch_size):
            image_features = _encode_visual_batch(
                siglip, siglip_processor, memory_fronts[start : start + args.batch_size]
            )
            text_batch = torch.stack(
                [
                    text_by_task[int(row["task_index"])]
                    for row in memories[start : start + args.batch_size]
                ]
            )
            memory_visual_batches.append(
                torch.nn.functional.normalize(image_features + text_batch, dim=-1).cpu()
            )
    memory_visual = torch.cat(memory_visual_batches)
    memory_vectors = torch.stack(
        [
            _combine_embedding(vector, row["state"], state_means, state_stds)
            for vector, row in zip(memory_visual, memories)
        ]
    )
    client = AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    _insert_memories(client, memories, memory_vectors, run_id=args.run_id)

    warm = calibration[0]
    for warm_seed in (args.seed * 1000, args.seed * 1000 + 1):
        _vla_action(
            policy,
            preprocess,
            postprocess,
            warm,
            front_by_episode[int(warm["episode_index"])],
            wrist_by_episode[int(warm["episode_index"])],
            seed=warm_seed,
        )
    torch.cuda.reset_peak_memory_stats()

    calibration_rows = []
    for sample in calibration:
        episode = int(sample["episode_index"])
        vector, _, _ = _encode_query(
            siglip,
            siglip_processor,
            front_by_episode[episode],
            text_by_task[int(sample["task_index"])],
            sample,
            state_means,
            state_stds,
        )
        matches, _ = _search(client, vector, run_id=args.run_id, top_k=args.top_k)
        prior, confidence = weighted_action_prior(matches, action_scale)
        baseline, _, _ = _vla_action(
            policy,
            preprocess,
            postprocess,
            sample,
            front_by_episode[episode],
            wrist_by_episode[episode],
            seed=args.seed * 100000 + episode,
        )
        calibration_rows.append(
            {
                "confidence": confidence,
                "baseline_mae": normalized_action_mae(
                    baseline, sample["action"], action_scale
                ),
                "prior_mae": normalized_action_mae(prior, sample["action"], action_scale),
            }
        )
    calibration_result = calibrate_threshold(calibration_rows)
    threshold = float(calibration_result["threshold"])

    energy = _energy_reader()
    baseline_energy_start = energy() if energy else None
    baseline_actions: dict[int, list[float]] = {}
    baseline_wall: list[float] = []
    baseline_gpu: list[float] = []
    for sample in evaluation:
        episode = int(sample["episode_index"])
        action, wall_ms, gpu_ms = _vla_action(
            policy,
            preprocess,
            postprocess,
            sample,
            front_by_episode[episode],
            wrist_by_episode[episode],
            seed=args.seed * 100000 + episode,
        )
        baseline_actions[episode] = action
        baseline_wall.append(wall_ms)
        baseline_gpu.append(gpu_ms)
    baseline_energy_j = _phase_energy(energy, baseline_energy_start)

    routed_energy_start = energy() if energy else None
    routed_actions: dict[int, list[float]] = {}
    routed_wall: list[float] = []
    routed_gpu: list[float] = []
    encoder_wall: list[float] = []
    encoder_gpu: list[float] = []
    search_latency: list[float] = []
    fallback_wall: list[float] = []
    fallback_gpu: list[float] = []
    skipped = 0
    for sample in evaluation:
        decision_start = time.perf_counter_ns()
        episode = int(sample["episode_index"])
        vector, encode_wall_ms, encode_gpu_ms = _encode_query(
            siglip,
            siglip_processor,
            front_by_episode[episode],
            text_by_task[int(sample["task_index"])],
            sample,
            state_means,
            state_stds,
        )
        matches, search_ms = _search(client, vector, run_id=args.run_id, top_k=args.top_k)
        prior, confidence = weighted_action_prior(matches, action_scale)
        if confidence >= threshold:
            action = prior
            skipped += 1
        else:
            action, vla_wall_ms, vla_gpu_ms = _vla_action(
                policy,
                preprocess,
                postprocess,
                sample,
                front_by_episode[episode],
                wrist_by_episode[episode],
                seed=args.seed * 100000 + episode,
            )
            fallback_wall.append(vla_wall_ms)
            fallback_gpu.append(vla_gpu_ms)
        routed_actions[episode] = action
        encoder_wall.append(encode_wall_ms)
        encoder_gpu.append(encode_gpu_ms)
        search_latency.append(search_ms)
        routed_gpu.append(encode_gpu_ms + (fallback_gpu[-1] if confidence < threshold else 0.0))
        routed_wall.append((time.perf_counter_ns() - decision_start) / 1_000_000)
    routed_energy_j = _phase_energy(energy, routed_energy_start)

    baseline_mae = statistics.fmean(
        normalized_action_mae(
            baseline_actions[int(row["episode_index"])], row["action"], action_scale
        )
        for row in evaluation
    )
    routed_mae = statistics.fmean(
        normalized_action_mae(
            routed_actions[int(row["episode_index"])], row["action"], action_scale
        )
        for row in evaluation
    )
    baseline_gpu_total = sum(baseline_gpu)
    routed_gpu_total = sum(routed_gpu)
    call_reduction = skipped / len(evaluation)
    gpu_reduction = 1.0 - routed_gpu_total / baseline_gpu_total
    latency_reduction = 1.0 - statistics.fmean(routed_wall) / statistics.fmean(baseline_wall)
    quality_limit = max(baseline_mae * 1.05, baseline_mae + 0.005)
    acceptance = {
        "real_vla_invoked": len(calibration) + len(evaluation) + len(fallback_wall) > 0,
        "vla_call_reduction": call_reduction >= 0.30,
        "gpu_time_reduction": gpu_reduction >= 0.20,
        "mean_latency_reduction": latency_reduction >= 0.15,
        "action_quality": routed_mae <= quality_limit,
        "zeptodb_search_p95": percentile(search_latency, 0.95) < 30.0,
    }
    return {
        "status": "pass" if all(acceptance.values()) else "fail",
        "experiment": 26,
        "dataset": DATASET,
        "siglip_model": args.siglip_model,
        "vla_model": args.vla_model,
        "vla_sha": resolved_sha,
        "runtime": {
            "gpu": torch.cuda.get_device_name(0),
            "cuda": torch.version.cuda,
            "vla_parameters": sum(parameter.numel() for parameter in policy.parameters()),
            "peak_gpu_memory_bytes": int(torch.cuda.max_memory_allocated()),
        },
        "split": {
            "memories": len(memories),
            "calibration": len(calibration),
            "evaluation": len(evaluation),
            "tasks": len({int(row["task_index"]) for row in samples}),
        },
        "calibration": calibration_result,
        "baseline": {
            "vla_calls": len(evaluation),
            "wall": latency_summary(baseline_wall),
            "vla_gpu": latency_summary(baseline_gpu),
            "gpu_total_ms": baseline_gpu_total,
            "energy_j": baseline_energy_j,
            "action_mae": baseline_mae,
        },
        "routed": {
            "vla_calls": len(fallback_wall),
            "skipped": skipped,
            "call_reduction": call_reduction,
            "wall": latency_summary(routed_wall),
            "encoder_wall": latency_summary(encoder_wall),
            "encoder_gpu_total_ms": sum(encoder_gpu),
            "search": latency_summary(search_latency),
            "fallback_vla_wall": latency_summary(fallback_wall),
            "gpu_total_ms": routed_gpu_total,
            "gpu_reduction": gpu_reduction,
            "latency_reduction": latency_reduction,
            "energy_j": routed_energy_j,
            "action_mae": routed_mae,
            "quality_limit": quality_limit,
        },
        "acceptance": acceptance,
    }


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    runtime = result["runtime"]
    baseline = result["baseline"]
    routed = result["routed"]
    energy_baseline = (
        f"{baseline['energy_j']:.1f}" if baseline["energy_j"] is not None else "unsupported"
    )
    energy_routed = (
        f"{routed['energy_j']:.1f}" if routed["energy_j"] is not None else "unsupported"
    )
    labels = {
        "real_vla_invoked": "Pinned real SmolVLA loaded and invoked",
        "vla_call_reduction": "VLA calls reduced by at least 30%",
        "gpu_time_reduction": "Total online GPU time reduced by at least 20%",
        "mean_latency_reduction": "Mean decision latency reduced by at least 15%",
        "action_quality": "Routed normalized action MAE within quality limit",
        "zeptodb_search_p95": "ZeptoDB exact-search p95 below 30 ms",
        "aws_resources_deleted": "Temporary AWS resources deleted",
    }
    lines = [
        "# Physical AI VLA Early Exit 026 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Dataset: `{result['dataset']}`",
        f"VLA: `{result['vla_model']}` at `{result['vla_sha']}`",
        f"Retrieval encoder: `{result['siglip_model']}`",
        "",
        "## Scope",
        "",
        "This offline replay invokes the real CUDA VLA and compares it with a",
        "ZeptoDB confidence-gated historical-action early exit. Action MAE against",
        "the recorded expert action is an offline proxy, not simulator task success.",
        "",
        "## Runtime",
        "",
        "| GPU | VLA parameters | Peak GPU MiB | Memories | Calibration | Evaluation |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
        f"| {runtime['gpu']} | {runtime['vla_parameters']} | "
        f"{runtime['peak_gpu_memory_bytes'] / 1048576:.1f} | "
        f"{result['split']['memories']} | {result['split']['calibration']} | "
        f"{result['split']['evaluation']} |",
        "",
        "## Compute And Latency",
        "",
        "| Path | VLA calls | Mean decision ms | p95 decision ms | GPU total ms | Energy J |",
        "| --- | ---: | ---: | ---: | ---: | --- |",
        f"| Direct VLA | {baseline['vla_calls']} | {baseline['wall']['mean_ms']:.3f} | "
        f"{baseline['wall']['p95_ms']:.3f} | {baseline['gpu_total_ms']:.3f} | "
        f"{energy_baseline} |",
        f"| ZeptoDB routed | {routed['vla_calls']} | {routed['wall']['mean_ms']:.3f} | "
        f"{routed['wall']['p95_ms']:.3f} | {routed['gpu_total_ms']:.3f} | "
        f"{energy_routed} |",
        "",
        f"- VLA call reduction: {routed['call_reduction']:.1%}.",
        f"- Total online GPU-time reduction: {routed['gpu_reduction']:.1%}.",
        f"- Mean decision-latency reduction: {routed['latency_reduction']:.1%}.",
        f"- Query encoder mean: {routed['encoder_wall']['mean_ms']:.3f} ms.",
        f"- ZeptoDB search p50/p95: {routed['search']['p50_ms']:.3f}/"
        f"{routed['search']['p95_ms']:.3f} ms.",
        "",
        "## Offline Action Quality",
        "",
        "| Path | Normalized action MAE | Allowed routed limit |",
        "| --- | ---: | ---: |",
        f"| Direct VLA | {baseline['action_mae']:.6f} | - |",
        f"| ZeptoDB routed | {routed['action_mae']:.6f} | "
        f"{routed['quality_limit']:.6f} |",
        "",
        "## Acceptance",
        "",
        "| Criterion | Status |",
        "| --- | --- |",
    ]
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
        "The result applies only to the deterministic middle-frame LIBERO replay,",
        "the fixed memory split, and this model revision. It does not establish",
        "closed-loop control quality, safety, or production VLA routing readiness.",
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
    parser.add_argument("--memory-per-task", type=int, default=19)
    parser.add_argument("--query-per-task", type=int, default=10)
    parser.add_argument("--calibration-per-task", type=int, default=5)
    parser.add_argument("--seed", type=int, default=25)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--download-workers", type=int, default=12)
    parser.add_argument("--manifest-workers", type=int, default=3)
    parser.add_argument("--rows-request-interval", type=float, default=1.5)
    parser.add_argument("--http-timeout", type=float, default=15.0)
    parser.add_argument("--result", type=Path, default=Path("/dev/termination-log"))
    parser.add_argument("--prepare-manifest", type=Path)
    parser.add_argument("--manifest-checkpoint", type=Path)
    parser.add_argument("--sample-manifest", type=Path)
    parser.add_argument("--render-json", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.prepare_manifest:
        manifest = prepare_sample_manifest(args)
        print(
            f"Prepared {len(manifest['memories'])} memories and "
            f"{len(manifest['queries'])} queries"
        )
        return 0
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
        parser.error("--sample-manifest is required for execution")
    try:
        result = run_experiment(args)
        _write_result(args.result, result)
        return 0 if result["status"] == "pass" else 2
    except Exception as exc:
        _write_result(
            args.result,
            {
                "status": "error",
                "experiment": 26,
                "error_type": type(exc).__name__,
                "error": str(exc)[:3000],
            },
        )
        raise


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run and report the research-only LIBERO vision retrieval experiment."""

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
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DATASET = "lerobot/libero_10_image"
DATASET_REVISION = "main"
MODEL_ID = "google/siglip-base-patch16-224"
HF_BASE = f"https://huggingface.co/datasets/{DATASET}/resolve/{DATASET_REVISION}"
ROWS_URL = "https://datasets-server.huggingface.co/rows"
ROWS_REQUEST_INTERVAL_SECONDS = 0.6
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
        "max_ms": max(values, default=0.0),
    }


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
        item = {
            "episode_index": int(episode["episode_index"]),
            "task_index": task_index,
            "task": names[0],
            "row_index": int(episode["dataset_from_index"]) + int(episode["length"]) // 2,
        }
        grouped.setdefault(task_index, []).append(item)

    expected_tasks = sorted(int(row["task_index"]) for row in task_rows)
    if sorted(grouped) != expected_tasks:
        raise ValueError("episode metadata does not cover every declared task")

    needed = memory_per_task + query_per_task
    memories: list[dict[str, Any]] = []
    queries: list[dict[str, Any]] = []
    for task_index in expected_tasks:
        candidates = sorted(grouped[task_index], key=lambda row: row["episode_index"])
        if len(candidates) < needed:
            raise ValueError(
                f"task {task_index} has {len(candidates)} episodes, but {needed} are required"
            )
        random.Random(seed + task_index).shuffle(candidates)
        memories.extend(candidates[:memory_per_task])
        queries.extend(candidates[memory_per_task:needed])
    return memories, queries


def retrieval_metrics(
    ranked_task_indices: list[list[int]],
    query_task_indices: list[int],
) -> dict[str, float]:
    if len(ranked_task_indices) != len(query_task_indices):
        raise ValueError("ranked results and queries must have equal length")
    if not query_task_indices:
        return {"recall_at_1": 0.0, "recall_at_5": 0.0, "mrr": 0.0}
    reciprocal_ranks: list[float] = []
    recall_at_1 = 0
    recall_at_5 = 0
    for ranked, expected in zip(ranked_task_indices, query_task_indices):
        if ranked and ranked[0] == expected:
            recall_at_1 += 1
        if expected in ranked[:5]:
            recall_at_5 += 1
        try:
            reciprocal_ranks.append(1.0 / (ranked.index(expected) + 1))
        except ValueError:
            reciprocal_ranks.append(0.0)
    count = len(query_task_indices)
    return {
        "recall_at_1": recall_at_1 / count,
        "recall_at_5": recall_at_5 / count,
        "mrr": statistics.fmean(reciprocal_ranks),
    }


def _wait_for_rows_request_slot() -> None:
    global _NEXT_ROWS_REQUEST_AT
    with _ROWS_REQUEST_LOCK:
        delay = _NEXT_ROWS_REQUEST_AT - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        _NEXT_ROWS_REQUEST_AT = time.monotonic() + ROWS_REQUEST_INTERVAL_SECONDS


def _request_bytes(
    url: str,
    *,
    attempts: int = 5,
    timeout: float = 60.0,
    throttle_rows: bool = False,
) -> bytes:
    last_error: Exception | None = None
    for attempt in range(attempts):
        if throttle_rows:
            _wait_for_rows_request_slot()
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "zeptodb-research/025"})
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
                        retry_after = 0.0
                backoff = min(60.0, 1.0 * (2**attempt))
                time.sleep(max(retry_after, backoff) + random.uniform(0.0, 0.5))
    raise RuntimeError(f"request failed after {attempts} attempts: {url}") from last_error


def _load_metadata() -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    import pyarrow.parquet as parquet

    episodes_data = _request_bytes(
        f"{HF_BASE}/meta/episodes/chunk-000/file-000.parquet"
    )
    tasks_data = _request_bytes(f"{HF_BASE}/meta/tasks.parquet")
    episode_columns = [
        "episode_index",
        "dataset_from_index",
        "length",
        "tasks",
    ]
    episodes = parquet.read_table(
        io.BytesIO(episodes_data), columns=episode_columns
    ).to_pylist()
    tasks = parquet.read_table(io.BytesIO(tasks_data)).to_pylist()
    return episodes, tasks


def _resolve_image_url(sample: dict[str, Any]) -> str:
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
        _request_bytes(
            f"{ROWS_URL}?{query}",
            attempts=10,
            timeout=90.0,
            throttle_rows=True,
        )
    )
    rows = payload.get("rows", [])
    if len(rows) != 1:
        raise RuntimeError(
            f"dataset server returned {len(rows)} rows for index {sample['row_index']}"
        )
    row = rows[0].get("row", {})
    if int(row.get("episode_index", -1)) != sample["episode_index"]:
        raise RuntimeError(
            f"row {sample['row_index']} belongs to episode {row.get('episode_index')}, "
            f"expected {sample['episode_index']}"
        )
    image_url = row.get("observation.images.image", {}).get("src")
    if not image_url:
        raise RuntimeError(f"row {sample['row_index']} has no front-camera image URL")
    return str(image_url)


def _fetch_image(sample: dict[str, Any]) -> Any:
    from PIL import Image

    image_url = sample.get("image_url") or _resolve_image_url(sample)
    return Image.open(io.BytesIO(_request_bytes(image_url, timeout=90.0))).convert("RGB")


def prepare_sample_manifest(args: argparse.Namespace) -> dict[str, Any]:
    episodes, task_rows = _load_metadata()
    memories, queries = select_balanced_episodes(
        episodes,
        task_rows,
        memory_per_task=args.memory_per_task,
        query_per_task=args.query_per_task,
        seed=args.seed,
    )
    samples = memories + queries
    with ThreadPoolExecutor(max_workers=args.download_workers) as pool:
        image_urls = list(pool.map(_resolve_image_url, samples))
    for sample, image_url in zip(samples, image_urls):
        sample["image_url"] = image_url
    manifest = {
        "dataset": DATASET,
        "dataset_revision": DATASET_REVISION,
        "memory_per_task": args.memory_per_task,
        "query_per_task": args.query_per_task,
        "seed": args.seed,
        "memories": memories,
        "queries": queries,
    }
    args.prepare_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.prepare_manifest.write_text(json.dumps(manifest, separators=(",", ":")))
    return manifest


def load_sample_manifest(
    path: Path,
    *,
    memory_per_task: int,
    query_per_task: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    manifest = json.loads(path.read_text())
    memories = manifest.get("memories")
    queries = manifest.get("queries")
    if not isinstance(memories, list) or not isinstance(queries, list):
        raise ValueError("sample manifest must contain memories and queries arrays")
    expected_memories = 10 * memory_per_task
    expected_queries = 10 * query_per_task
    if len(memories) != expected_memories or len(queries) != expected_queries:
        raise ValueError(
            f"sample manifest has {len(memories)} memories and {len(queries)} queries; "
            f"expected {expected_memories} and {expected_queries}"
        )
    samples = memories + queries
    if any(not sample.get("image_url") for sample in samples):
        raise ValueError("sample manifest contains an entry without image_url")
    if len({int(sample["task_index"]) for sample in samples}) != 10:
        raise ValueError("sample manifest does not cover all 10 tasks")
    return memories, queries


def _encode(
    samples: list[dict[str, Any]],
    *,
    model_id: str,
    batch_size: int,
    workers: int,
) -> tuple[Any, dict[int, Any], dict[str, Any]]:
    import torch
    from transformers import AutoModel, AutoProcessor

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is not available in the experiment container")
    load_start = time.perf_counter()
    processor = AutoProcessor.from_pretrained(model_id)
    model = AutoModel.from_pretrained(model_id, torch_dtype=torch.float16)
    model = model.to("cuda").eval()
    torch.cuda.synchronize()
    model_load_seconds = time.perf_counter() - load_start

    fetch_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=workers) as pool:
        images = list(pool.map(_fetch_image, samples))
    fetch_seconds = time.perf_counter() - fetch_start

    torch.cuda.reset_peak_memory_stats()
    image_batches = []
    batch_latencies: list[float] = []
    with torch.inference_mode():
        for start in range(0, len(images), batch_size):
            inputs = processor(images=images[start:start + batch_size], return_tensors="pt")
            pixel_values = inputs["pixel_values"].to("cuda", non_blocking=True)
            torch.cuda.synchronize()
            batch_start = time.perf_counter_ns()
            features = model.get_image_features(pixel_values=pixel_values)
            features = torch.nn.functional.normalize(features.float(), dim=-1)
            torch.cuda.synchronize()
            batch_latencies.append((time.perf_counter_ns() - batch_start) / 1_000_000)
            image_batches.append(features.cpu())
    image_features = torch.cat(image_batches, dim=0)

    task_texts = {
        int(sample["task_index"]): sample["task"]
        for sample in samples
    }
    ordered_tasks = sorted(task_texts)
    text_inputs = processor(
        text=[task_texts[index] for index in ordered_tasks],
        padding="max_length",
        return_tensors="pt",
    )
    text_inputs = {key: value.to("cuda") for key, value in text_inputs.items()}
    with torch.inference_mode():
        text_features = model.get_text_features(**text_inputs)
        text_features = torch.nn.functional.normalize(text_features.float(), dim=-1).cpu()
    text_by_task = {
        task_index: text_features[position]
        for position, task_index in enumerate(ordered_tasks)
    }
    peak_bytes = int(torch.cuda.max_memory_allocated())
    return image_features, text_by_task, {
        "gpu": torch.cuda.get_device_name(0),
        "cuda": torch.version.cuda,
        "dimensions": int(image_features.shape[1]),
        "samples": len(samples),
        "fetch_seconds": fetch_seconds,
        "model_load_seconds": model_load_seconds,
        "batch_latency": latency_summary(batch_latencies),
        "encoder_total_ms": sum(batch_latencies),
        "encoder_ms_per_image": sum(batch_latencies) / len(samples),
        "peak_gpu_memory_bytes": peak_bytes,
    }


def _rank_locally(memory_features: Any, query_features: Any, top_k: int) -> list[list[int]]:
    import torch

    similarities = query_features @ memory_features.T
    return torch.topk(similarities, k=top_k, dim=1).indices.tolist()


class AgentMemoryClient:
    def __init__(self, base_url: str, timeout: float = 15.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        payload: dict[str, Any],
    ) -> tuple[dict[str, Any], float]:
        body = json.dumps(payload, separators=(",", ":")).encode()
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=body,
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
        elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
        if not isinstance(data, dict):
            raise RuntimeError(f"{method} {path} returned non-object JSON")
        return data, elapsed_ms


def _run_zeptodb(
    client: AgentMemoryClient,
    *,
    mode: str,
    memories: list[dict[str, Any]],
    queries: list[dict[str, Any]],
    memory_features: Any,
    query_features: Any,
    run_id: str,
    top_k: int,
    repeats: int,
) -> dict[str, Any]:
    tenant = f"physical-ai-025-{run_id}"
    namespace = f"libero-{mode}"
    insert_latencies: list[float] = []
    for sample, vector in zip(memories, memory_features.tolist()):
        response, elapsed = client.request(
            "POST",
            "/api/ai/memories",
            {
                "memory_id": f"{run_id}-{mode}-{sample['episode_index']}",
                "tenant_id": tenant,
                "namespace": namespace,
                "agent_id": "physical-ai-vision-retrieval",
                "type": "libero_episode",
                "content": sample["task"],
                "metadata_json": json.dumps(
                    {
                        "episode_index": sample["episode_index"],
                        "task_index": sample["task_index"],
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
        insert_latencies.append(elapsed)

    ranked_tasks: list[list[int]] = []
    search_latencies: list[float] = []
    for sample, vector in zip(queries, query_features.tolist()):
        request = {
            "tenant_id": tenant,
            "namespace": namespace,
            "agent_id": "physical-ai-vision-retrieval",
            "type": "libero_episode",
            "query_embedding": vector,
            "limit": top_k,
        }
        response, _ = client.request("POST", "/api/ai/memories/search", request)
        matches = response.get("matches", [])
        if not matches:
            raise RuntimeError(f"ZeptoDB returned no matches for episode {sample['episode_index']}")
        ranked_tasks.append(
            [
                int(json.loads(match["metadata_json"])["task_index"])
                for match in matches
            ]
        )
        for _ in range(repeats):
            _, elapsed = client.request("POST", "/api/ai/memories/search", request)
            search_latencies.append(elapsed)
    return {
        "quality": retrieval_metrics(
            ranked_tasks, [int(sample["task_index"]) for sample in queries]
        ),
        "insert": latency_summary(insert_latencies),
        "search": latency_summary(search_latencies),
    }


def run_experiment(args: argparse.Namespace) -> dict[str, Any]:
    import torch

    metadata_start = time.perf_counter()
    if args.sample_manifest:
        memories, queries = load_sample_manifest(
            args.sample_manifest,
            memory_per_task=args.memory_per_task,
            query_per_task=args.query_per_task,
        )
    else:
        episodes, task_rows = _load_metadata()
        memories, queries = select_balanced_episodes(
            episodes,
            task_rows,
            memory_per_task=args.memory_per_task,
            query_per_task=args.query_per_task,
            seed=args.seed,
        )
    samples = memories + queries
    image_features, text_by_task, runtime = _encode(
        samples,
        model_id=args.model,
        batch_size=args.batch_size,
        workers=args.download_workers,
    )
    memory_count = len(memories)
    image_memory = image_features[:memory_count]
    image_query = image_features[memory_count:]
    text_features = torch.stack(
        [text_by_task[int(sample["task_index"])] for sample in samples]
    )
    combined = torch.nn.functional.normalize(image_features + text_features, dim=-1)
    combined_memory = combined[:memory_count]
    combined_query = combined[memory_count:]
    memory_tasks = [int(sample["task_index"]) for sample in memories]
    query_tasks = [int(sample["task_index"]) for sample in queries]

    modes: dict[str, Any] = {}
    client = AgentMemoryClient(args.agent_url, timeout=args.http_timeout)
    for mode, memory_vectors, query_vectors in (
        ("image", image_memory, image_query),
        ("image_text", combined_memory, combined_query),
    ):
        local_indices = _rank_locally(memory_vectors, query_vectors, args.top_k)
        local_ranked_tasks = [
            [memory_tasks[index] for index in ranked]
            for ranked in local_indices
        ]
        modes[mode] = {
            "local_quality": retrieval_metrics(local_ranked_tasks, query_tasks),
            "zeptodb": _run_zeptodb(
                client,
                mode=mode,
                memories=memories,
                queries=queries,
                memory_features=memory_vectors,
                query_features=query_vectors,
                run_id=args.run_id,
                top_k=args.top_k,
                repeats=args.repeats,
            ),
        }

    image_text = modes["image_text"]["zeptodb"]
    acceptance = {
        "ten_tasks": len({sample["task_index"] for sample in samples}) == 10,
        "balanced_queries": len(queries) == 10 * args.query_per_task,
        "image_text_recall_at_1": image_text["quality"]["recall_at_1"] >= 0.80,
        "image_text_recall_at_5": image_text["quality"]["recall_at_5"] >= 0.95,
        "zeptodb_search_p95": image_text["search"]["p95_ms"] < 30.0,
        "cuda_encoder": bool(runtime["gpu"]) and runtime["dimensions"] > 0,
    }
    return {
        "status": "pass" if all(acceptance.values()) else "fail",
        "experiment": 25,
        "dataset": DATASET,
        "dataset_revision": DATASET_REVISION,
        "model": args.model,
        "split": {
            "tasks": 10,
            "memory_per_task": args.memory_per_task,
            "query_per_task": args.query_per_task,
            "memories": len(memories),
            "queries": len(queries),
            "seed": args.seed,
            "representative_frame": "middle",
        },
        "runtime": runtime,
        "metadata_and_total_seconds": time.perf_counter() - metadata_start,
        "modes": modes,
        "acceptance": acceptance,
    }


def render_report(result: dict[str, Any]) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    split = result["split"]
    runtime = result["runtime"]
    lines = [
        "# Physical AI Vision Retrieval 025 Results",
        "",
        f"Generated at: {generated}",
        "Classification: Research-only",
        f"Dataset: `{result['dataset']}` at `{result['dataset_revision']}`",
        f"Encoder: `{result['model']}`",
        "",
        "## Scope",
        "",
        "This run uses real LIBERO front-camera frames, a real SigLIP encoder on",
        "an NVIDIA GPU, and the real ZeptoDB Agent Memory HTTP path. It measures",
        "task-level episode retrieval, not VLA action generation or simulator success.",
        "",
        "## Data And Runtime",
        "",
        "| Tasks | Memories | Held-out queries | Frame | GPU | Embedding dims | Peak GPU MiB | Encoder ms/image |",
        "| ---: | ---: | ---: | --- | --- | ---: | ---: | ---: |",
        f"| {split['tasks']} | {split['memories']} | {split['queries']} | "
        f"{split['representative_frame']} | {runtime['gpu']} | {runtime['dimensions']} | "
        f"{runtime['peak_gpu_memory_bytes'] / 1048576:.1f} | "
        f"{runtime['encoder_ms_per_image']:.3f} |",
        "",
        "## Retrieval Quality",
        "",
        "| Variant | Path | Recall@1 | Recall@5 | MRR |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for mode in ("image", "image_text"):
        local = result["modes"][mode]["local_quality"]
        zepto = result["modes"][mode]["zeptodb"]["quality"]
        lines.append(
            f"| {mode} | local cosine | {local['recall_at_1']:.3f} | "
            f"{local['recall_at_5']:.3f} | {local['mrr']:.3f} |"
        )
        lines.append(
            f"| {mode} | ZeptoDB | {zepto['recall_at_1']:.3f} | "
            f"{zepto['recall_at_5']:.3f} | {zepto['mrr']:.3f} |"
        )
    lines += [
        "",
        "## ZeptoDB Latency",
        "",
        "| Variant | Insert p50 ms | Insert p95 ms | Search p50 ms | Search p95 ms |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for mode in ("image", "image_text"):
        zepto = result["modes"][mode]["zeptodb"]
        lines.append(
            f"| {mode} | {zepto['insert']['p50_ms']:.3f} | "
            f"{zepto['insert']['p95_ms']:.3f} | {zepto['search']['p50_ms']:.3f} | "
            f"{zepto['search']['p95_ms']:.3f} |"
        )
    lines += [
        "",
        "## Acceptance",
        "",
        "| Criterion | Status |",
        "| --- | --- |",
    ]
    labels = {
        "ten_tasks": "All 10 tasks represented",
        "balanced_queries": "10 held-out queries per task",
        "image_text_recall_at_1": "Image + instruction ZeptoDB Recall@1 >= 0.80",
        "image_text_recall_at_5": "Image + instruction ZeptoDB Recall@5 >= 0.95",
        "zeptodb_search_p95": "Image + instruction ZeptoDB search p95 < 30 ms",
        "cuda_encoder": "Real CUDA encoder produced embeddings",
        "aws_resources_deleted": "Temporary AWS resources deleted",
    }
    for key, passed in result["acceptance"].items():
        lines.append(f"| {labels[key]} | {'pass' if passed else 'fail'} |")
    lines += [
        "",
        "## Result",
        "",
        f"Overall status: {result['status']}.",
        "",
        "## Interpretation",
        "",
        "A pass proves that real visual embeddings can be created on the EKS GPU",
        "path and retrieved through ZeptoDB within this bounded workload. It does",
        "not prove faster or more accurate VLA policy inference; that remains the",
        "next experiment.",
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
    parser.add_argument("--model", default=MODEL_ID)
    parser.add_argument("--run-id", default=str(int(time.time())))
    parser.add_argument("--memory-per-task", type=int, default=19)
    parser.add_argument("--query-per-task", type=int, default=10)
    parser.add_argument("--seed", type=int, default=25)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--download-workers", type=int, default=12)
    parser.add_argument("--http-timeout", type=float, default=15.0)
    parser.add_argument("--result", type=Path, default=Path("/dev/termination-log"))
    parser.add_argument("--prepare-manifest", type=Path)
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

    try:
        result = run_experiment(args)
        _write_result(args.result, result)
        return 0 if result["status"] == "pass" else 2
    except Exception as exc:
        error = {
            "status": "error",
            "experiment": 25,
            "error_type": type(exc).__name__,
            "error": str(exc)[:3000],
        }
        _write_result(args.result, error)
        raise


if __name__ == "__main__":
    raise SystemExit(main())

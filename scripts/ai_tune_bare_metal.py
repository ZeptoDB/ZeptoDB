#!/usr/bin/env python3.13
"""
ZeptoDB AI-Driven Bare Metal Tuner
Uses claude-opus-4-6 with extended thinking to analyze the system,
run ZeptoDB benchmarks, and iteratively apply optimizations.

Usage:
    python3.13 scripts/ai_tune_bare_metal.py --build-dir build
    python3.13 scripts/ai_tune_bare_metal.py --build-dir build --dry-run
    python3.13 scripts/ai_tune_bare_metal.py --build-dir build --iterations 3
    sudo python3.13 scripts/ai_tune_bare_metal.py --build-dir build --apply-root
"""

import argparse
import json
import os
import re
import subprocess
import sys
import textwrap
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

import anthropic

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Use Bedrock model ID when running on AWS (no ANTHROPIC_API_KEY needed)
MODEL = os.environ.get("ANTHROPIC_DEFAULT_OPUS_MODEL", "us.anthropic.claude-opus-4-6-v1")
BENCHMARK_BINARY = "tests/zepto_tests"
BENCHMARK_FILTER = "Benchmark.*"

THINKING_BUDGET = 8000   # tokens for extended thinking

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class BenchmarkResult:
    xbar_1m_ms:      Optional[float] = None
    ema_1m_ms:       Optional[float] = None
    windowjoin_ms:   Optional[float] = None
    hugepages_active: bool = False
    raw_output:      str = ""

    def summary(self) -> str:
        lines = []
        if self.xbar_1m_ms is not None:
            lines.append(f"  Xbar 1M rows:        {self.xbar_1m_ms:.2f} ms")
        if self.ema_1m_ms is not None:
            lines.append(f"  EMA 1M rows:         {self.ema_1m_ms:.3f} ms")
        if self.windowjoin_ms is not None:
            lines.append(f"  Window JOIN 100K^2:  {self.windowjoin_ms:.2f} ms")
        lines.append(f"  Hugepages active:    {self.hugepages_active}")
        return "\n".join(lines)

    def vs(self, other: "BenchmarkResult") -> str:
        """Return improvement string compared to `other` (the baseline)."""
        lines = []
        def delta(label, new_val, old_val):
            if new_val is None or old_val is None:
                return
            pct = (old_val - new_val) / old_val * 100
            sign = "+" if pct > 0 else ""
            lines.append(f"  {label}: {old_val:.3f} -> {new_val:.3f} ms  ({sign}{pct:.1f}%)")
        delta("Xbar 1M     ", self.xbar_1m_ms,    other.xbar_1m_ms)
        delta("EMA 1M      ", self.ema_1m_ms,      other.ema_1m_ms)
        delta("Window JOIN ", self.windowjoin_ms,  other.windowjoin_ms)
        return "\n".join(lines) if lines else "  (no comparable metrics)"


@dataclass
class TuningCommand:
    cmd:           str
    description:   str
    requires_root: bool = False
    rollback:      str  = ""


@dataclass
class TuningPlan:
    analysis:  str
    commands:  list[TuningCommand] = field(default_factory=list)
    next_focus: str = ""


@dataclass
class SystemProfile:
    cpu_model:         str = ""
    cpu_cores:         int = 0
    cpu_governor:      str = ""
    turbo_boost:       str = ""
    c_states:          str = ""
    numa_nodes:        int = 0
    mem_total_mb:      int = 0
    mem_free_mb:       int = 0
    hugepages_total:   int = 0
    hugepages_free:    int = 0
    hugepage_size_kb:  int = 0
    thp_enabled:       str = ""
    swappiness:        int = -1
    numa_balancing:    int = -1
    network_bufs:      dict = field(default_factory=dict)
    irq_affinity:      dict = field(default_factory=dict)
    kernel_cmdline:    str = ""
    cpu_isolation:     bool = False
    os_release:        str = ""

# ---------------------------------------------------------------------------
# System profiling
# ---------------------------------------------------------------------------

def _read(path: str, default: str = "") -> str:
    try:
        return Path(path).read_text().strip()
    except Exception:
        return default


def _shell(cmd: str, default: str = "") -> str:
    try:
        return subprocess.check_output(cmd, shell=True, stderr=subprocess.DEVNULL,
                                       text=True).strip()
    except Exception:
        return default


def gather_system_profile() -> SystemProfile:
    p = SystemProfile()

    # CPU
    p.cpu_model = _shell("grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2").strip()
    p.cpu_cores = int(_shell("nproc", "0") or "0")
    p.cpu_governor = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "unknown")

    # Turbo boost
    if Path("/sys/devices/system/cpu/intel_pstate/no_turbo").exists():
        val = _read("/sys/devices/system/cpu/intel_pstate/no_turbo")
        p.turbo_boost = "disabled" if val == "1" else "enabled"
    elif Path("/sys/devices/system/cpu/cpufreq/boost").exists():
        val = _read("/sys/devices/system/cpu/cpufreq/boost")
        p.turbo_boost = "enabled" if val == "1" else "disabled"
    else:
        p.turbo_boost = "unknown"

    # C-states
    disabled = 0
    total = 0
    for state in Path("/sys/devices/system/cpu/cpu0/cpuidle").glob("state*/disable"):
        total += 1
        if _read(str(state)) == "1":
            disabled += 1
    p.c_states = f"{disabled}/{total} deep states disabled" if total else "unknown"

    # NUMA
    p.numa_nodes = int(_shell("numactl --hardware 2>/dev/null | grep 'available:' | awk '{print $2}'", "1") or "1")

    # Memory
    meminfo = _shell("cat /proc/meminfo")
    def memval(key):
        m = re.search(rf"^{key}:\s+(\d+)", meminfo, re.MULTILINE)
        return int(m.group(1)) // 1024 if m else 0
    p.mem_total_mb       = memval("MemTotal")
    p.mem_free_mb        = memval("MemAvailable")
    p.hugepages_total    = int(_shell("cat /proc/sys/vm/nr_hugepages", "0") or "0")
    p.hugepages_free     = memval("HugePages_Free")
    p.hugepage_size_kb   = memval("Hugepagesize") * 1024  # Hugepagesize is in kB
    p.thp_enabled        = _read("/sys/kernel/mm/transparent_hugepage/enabled", "unknown")

    # Kernel parameters
    p.swappiness     = int(_shell("sysctl -n vm.swappiness", "-1") or "-1")
    p.numa_balancing = int(_shell("sysctl -n kernel.numa_balancing", "-1") or "-1")

    # Network
    for key in ["net.core.busy_poll", "net.core.rmem_max", "net.core.wmem_max",
                "net.core.netdev_max_backlog"]:
        val = _shell(f"sysctl -n {key} 2>/dev/null")
        if val:
            p.network_bufs[key] = val

    # IRQ affinity (first few network IRQs)
    irq_lines = _shell("grep -E 'eth|mlx|ixgbe|enp' /proc/interrupts 2>/dev/null | head -5")
    if irq_lines:
        for line in irq_lines.splitlines():
            irq_num = line.strip().split(":")[0].strip()
            aff = _read(f"/proc/irq/{irq_num}/smp_affinity", "")
            if aff:
                p.irq_affinity[irq_num] = aff

    # Kernel cmdline
    p.kernel_cmdline = _read("/proc/cmdline")
    p.cpu_isolation  = "isolcpus" in p.kernel_cmdline

    # OS
    p.os_release = _shell("grep PRETTY_NAME /etc/os-release | cut -d= -f2 | tr -d '\"'")

    return p


# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def run_benchmarks(build_dir: str) -> BenchmarkResult:
    binary = Path(build_dir) / BENCHMARK_BINARY
    if not binary.exists():
        print(f"  [WARN] Benchmark binary not found: {binary}")
        return BenchmarkResult()

    print(f"  Running: {binary} --gtest_filter={BENCHMARK_FILTER}")
    t0 = time.time()
    try:
        output = subprocess.check_output(
            [str(binary), f"--gtest_filter={BENCHMARK_FILTER}"],
            stderr=subprocess.STDOUT, text=True, timeout=120
        )
    except subprocess.CalledProcessError as e:
        output = e.output
    except subprocess.TimeoutExpired:
        return BenchmarkResult(raw_output="TIMEOUT")

    elapsed = time.time() - t0
    print(f"  Benchmark wall time: {elapsed:.1f}s")

    result = BenchmarkResult(raw_output=output)

    m = re.search(r"xbar GROUP BY 1M rows:\s*([\d.]+)ms", output)
    if m:
        result.xbar_1m_ms = float(m.group(1))

    m = re.search(r"EMA 1M rows:\s*([\d.]+)ms", output)
    if m:
        result.ema_1m_ms = float(m.group(1))

    m = re.search(r"Window JOIN 100K.100K:\s*([\d.]+)ms", output)
    if m:
        result.windowjoin_ms = float(m.group(1))

    result.hugepages_active = "HugePages mmap failed" not in output

    return result


# ---------------------------------------------------------------------------
# Claude Opus 4.6 interaction
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are an expert Linux bare-metal performance engineer specializing in ultra-low latency systems for HFT (high-frequency trading).
You are tuning ZeptoDB, a C++20 in-memory columnar database running on Linux.

Your job:
1. Analyze the system profile and benchmark results provided.
2. Identify the highest-impact tuning opportunities.
3. Return a JSON object with your analysis and concrete shell commands to apply.

RULES:
- Prioritize tuning that reduces latency for memory-intensive workloads (column scans, SIMD operations).
- Hugepages are the highest priority: if hugepages are not allocated, fix that first.
- Only suggest commands that are safe, reversible, and well-understood.
- If root is required, set "requires_root": true.
- Always include a "rollback" command to undo each change.
- Limit to the 5 most impactful commands per iteration.
- Do NOT suggest rebooting or kernel parameter changes that require a reboot unless absolutely necessary.

OUTPUT FORMAT (JSON only, no markdown):
{
  "analysis": "concise explanation of bottlenecks found",
  "commands": [
    {
      "cmd": "the exact shell command to run",
      "description": "what this does and why",
      "requires_root": true,
      "rollback": "command to undo this"
    }
  ],
  "next_focus": "what to tackle in the next iteration"
}
"""


def ask_claude(
    client,
    profile: SystemProfile,
    baseline: BenchmarkResult,
    current: BenchmarkResult,
    iteration: int,
    history: list[str],
) -> TuningPlan:
    profile_json = json.dumps(asdict(profile), indent=2)
    baseline_json = json.dumps(asdict(baseline), indent=2)
    current_json  = json.dumps(asdict(current),  indent=2)

    history_section = ""
    if history:
        history_section = "\n\nPREVIOUS TUNING ACTIONS APPLIED:\n" + "\n".join(
            f"  Iteration {i+1}: {h}" for i, h in enumerate(history)
        )

    user_msg = textwrap.dedent(f"""\
        ITERATION: {iteration}

        SYSTEM PROFILE:
        {profile_json}

        BASELINE BENCHMARKS (before any tuning):
        {baseline_json}

        CURRENT BENCHMARKS:
        {current_json}

        BENCHMARK IMPROVEMENT SO FAR:
        {current.vs(baseline)}
        {history_section}

        Analyze and provide the next set of tuning commands.
        Respond with JSON only.
    """)

    print(f"\n  Calling {MODEL} (extended thinking, budget={THINKING_BUDGET} tokens)...")
    t0 = time.time()

    response = client.messages.create(
        model=MODEL,
        max_tokens=16000,
        thinking={
            "type": "enabled",
            "budget_tokens": THINKING_BUDGET,
        },
        system=SYSTEM_PROMPT,
        messages=[{"role": "user", "content": user_msg}],
    )

    elapsed = time.time() - t0
    print(f"  Claude response time: {elapsed:.1f}s")

    # Extract thinking block (optional display)
    thinking_text = ""
    response_text = ""
    for block in response.content:
        if block.type == "thinking":
            thinking_text = block.thinking
        elif block.type == "text":
            response_text = block.text

    if thinking_text:
        print("\n  [Claude thinking summary]")
        # Print first 500 chars of thinking
        preview = thinking_text[:500].replace("\n", "\n  ")
        print(f"  {preview}...")

    # Parse JSON from response
    json_text = response_text.strip()
    # Strip markdown code fences if present
    json_text = re.sub(r"^```(?:json)?\s*", "", json_text)
    json_text = re.sub(r"\s*```$", "", json_text)

    try:
        data = json.loads(json_text)
    except json.JSONDecodeError as e:
        print(f"  [WARN] Failed to parse Claude JSON: {e}")
        print(f"  Raw response: {response_text[:500]}")
        return TuningPlan(analysis="Parse error", commands=[], next_focus="")

    commands = [
        TuningCommand(
            cmd=c.get("cmd", ""),
            description=c.get("description", ""),
            requires_root=c.get("requires_root", False),
            rollback=c.get("rollback", ""),
        )
        for c in data.get("commands", [])
        if c.get("cmd")
    ]

    return TuningPlan(
        analysis=data.get("analysis", ""),
        commands=commands,
        next_focus=data.get("next_focus", ""),
    )


# ---------------------------------------------------------------------------
# Command application
# ---------------------------------------------------------------------------

def apply_commands(
    plan: TuningPlan,
    dry_run: bool,
    apply_root: bool,
) -> list[str]:
    applied = []
    is_root = (os.geteuid() == 0)

    print(f"\n  Analysis: {plan.analysis}")
    print(f"\n  Commands to apply ({len(plan.commands)}):")

    for i, cmd in enumerate(plan.commands, 1):
        root_tag = " [ROOT]" if cmd.requires_root else ""
        print(f"\n  [{i}] {cmd.description}{root_tag}")
        print(f"       CMD: {cmd.cmd}")
        if cmd.rollback:
            print(f"       UNDO: {cmd.rollback}")

        if dry_run:
            print("       STATUS: [DRY-RUN — skipped]")
            applied.append(f"[dry-run] {cmd.description}")
            continue

        if cmd.requires_root and not is_root and not apply_root:
            print("       STATUS: [SKIPPED — requires root; use --apply-root or run with sudo]")
            continue

        try:
            result = subprocess.run(
                cmd.cmd, shell=True, capture_output=True, text=True, timeout=30
            )
            if result.returncode == 0:
                print(f"       STATUS: OK")
                applied.append(cmd.description)
            else:
                print(f"       STATUS: FAILED (exit {result.returncode}): {result.stderr.strip()[:200]}")
        except subprocess.TimeoutExpired:
            print("       STATUS: TIMEOUT")
        except Exception as e:
            print(f"       STATUS: ERROR: {e}")

    return applied


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_section(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print(f"{'='*60}")


def print_benchmark_comparison(baseline: BenchmarkResult, current: BenchmarkResult):
    print("\n  Metric              Baseline       Current        Delta")
    print("  " + "-"*56)

    def row(label, base, cur):
        if base is None or cur is None:
            return
        pct = (base - cur) / base * 100
        sign = "+" if pct > 0 else ""
        print(f"  {label:<20} {base:>8.3f} ms    {cur:>8.3f} ms    {sign}{pct:.1f}%")

    row("Xbar 1M rows",    baseline.xbar_1m_ms,   current.xbar_1m_ms)
    row("EMA 1M rows",     baseline.ema_1m_ms,    current.ema_1m_ms)
    row("Window JOIN",     baseline.windowjoin_ms, current.windowjoin_ms)
    print(f"\n  Hugepages:  baseline={baseline.hugepages_active}  current={current.hugepages_active}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="ZeptoDB AI-Driven Bare Metal Tuner using claude-opus-4-6"
    )
    parser.add_argument("--build-dir", default="build",
                        help="Path to ZeptoDB build directory (default: build)")
    parser.add_argument("--iterations", type=int, default=2,
                        help="Number of tuning iterations (default: 2)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show recommended commands but do not apply them")
    parser.add_argument("--apply-root", action="store_true",
                        help="Apply root-required commands (or run with sudo)")
    parser.add_argument("--api-key",
                        help="Anthropic API key (default: ANTHROPIC_API_KEY env var)")
    parser.add_argument("--no-benchmark", action="store_true",
                        help="Skip benchmark runs (use zero values, for testing)")
    parser.add_argument("--thinking-budget", type=int, default=THINKING_BUDGET,
                        help=f"Extended thinking token budget (default: {THINKING_BUDGET})")
    args = parser.parse_args()

    api_key = args.api_key or os.environ.get("ANTHROPIC_API_KEY")
    if api_key:
        client = anthropic.Anthropic(api_key=api_key)
    else:
        # Use AWS Bedrock (picks up IAM role / instance profile automatically)
        client = anthropic.AnthropicBedrock()
    build_dir = str(Path(args.build_dir).resolve())

    print_section(f"ZeptoDB AI Bare Metal Tuner  |  model: {MODEL}")
    print(f"  Build dir:   {build_dir}")
    print(f"  Iterations:  {args.iterations}")
    print(f"  Dry-run:     {args.dry_run}")
    print(f"  Apply root:  {args.apply_root}")
    print(f"  Is root:     {os.geteuid() == 0}")

    # 1. Profile system
    print_section("Step 1: Profiling system")
    profile = gather_system_profile()
    print(f"  CPU:         {profile.cpu_model}")
    print(f"  Cores:       {profile.cpu_cores}")
    print(f"  Memory:      {profile.mem_total_mb} MB total, {profile.mem_free_mb} MB free")
    print(f"  NUMA nodes:  {profile.numa_nodes}")
    print(f"  Hugepages:   {profile.hugepages_total} allocated ({profile.hugepage_size_kb // 1024} MB each)")
    print(f"  THP:         {profile.thp_enabled}")
    print(f"  Governor:    {profile.cpu_governor}")
    print(f"  Turbo boost: {profile.turbo_boost}")
    print(f"  C-states:    {profile.c_states}")
    print(f"  Swappiness:  {profile.swappiness}")
    print(f"  isolcpus:    {profile.cpu_isolation}")

    # 2. Baseline benchmark
    print_section("Step 2: Baseline benchmarks")
    if args.no_benchmark:
        baseline = BenchmarkResult(xbar_1m_ms=45.0, ema_1m_ms=2.2, windowjoin_ms=10.0)
        print("  [--no-benchmark] Using placeholder values")
    else:
        baseline = run_benchmarks(build_dir)
    print(baseline.summary())

    current = baseline
    history: list[str] = []
    all_results: list[tuple[int, BenchmarkResult]] = [(0, baseline)]

    # 3. Tuning iterations
    for iteration in range(1, args.iterations + 1):
        print_section(f"Iteration {iteration}/{args.iterations}: AI Analysis + Tuning")

        plan = ask_claude(
            client=client,
            profile=profile,
            baseline=baseline,
            current=current,
            iteration=iteration,
            history=history,
        )

        if not plan.commands:
            print("  Claude returned no commands. System may already be well-tuned.")
            break

        applied = apply_commands(plan, dry_run=args.dry_run, apply_root=args.apply_root)
        history.extend(applied)

        if plan.next_focus:
            print(f"\n  Next iteration focus: {plan.next_focus}")

        # Re-benchmark after applying
        if not args.dry_run and applied:
            print_section(f"Post-iteration {iteration} benchmarks")
            current = run_benchmarks(build_dir) if not args.no_benchmark else current
            print(current.summary())
            all_results.append((iteration, current))
        elif args.dry_run:
            print("\n  [DRY-RUN] Skipping post-iteration benchmark")

    # 4. Final report
    print_section("Final Report")
    print_benchmark_comparison(baseline, current)

    if len(all_results) > 1:
        print("\n  Iteration history:")
        for iter_n, r in all_results:
            tag = "baseline" if iter_n == 0 else f"iter {iter_n}"
            xbar = f"{r.xbar_1m_ms:.2f}ms" if r.xbar_1m_ms else "N/A"
            ema  = f"{r.ema_1m_ms:.3f}ms" if r.ema_1m_ms else "N/A"
            wj   = f"{r.windowjoin_ms:.2f}ms" if r.windowjoin_ms else "N/A"
            print(f"    [{tag:10}]  xbar={xbar}  ema={ema}  wjoin={wj}  hugepages={r.hugepages_active}")

    if history:
        print(f"\n  Applied tuning ({len(history)} actions):")
        for h in history:
            print(f"    - {h}")

    print(f"\n  Run with --apply-root (or sudo) to apply root-required optimizations.")
    print(f"  Re-run with --iterations 1 for further incremental tuning.\n")


if __name__ == "__main__":
    main()

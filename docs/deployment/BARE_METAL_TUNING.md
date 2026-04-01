# ZeptoDB Bare Metal Tuning Guide

> Detailed guide for extracting maximum performance from ZeptoDB on bare-metal servers.
> For cloud/K8s tuning, see [CLOUD_PERFORMANCE_TUNING.md](CLOUD_PERFORMANCE_TUNING.md).

**Last Updated:** 2026-03-31
**Tested On:** Intel Xeon 6975P-C (8C), 31GB RAM, Amazon Linux 2023

---

## Quick Start

```bash
# 1. Kernel boot parameters (reboot required)
sudo vi /etc/default/grub
# Add: isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3 transparent_hugepage=never \
#      processor.max_cstate=1 intel_idle.max_cstate=0 \
#      default_hugepagesz=2M hugepagesz=2M hugepages=16384
sudo grub2-mkconfig -o /boot/grub2/grub.cfg && sudo reboot

# 2. Runtime tuning
sudo ./deploy/scripts/tune_bare_metal.sh

# 3. Build (production)
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DZEPTO_USE_TCMALLOC=ON -DZEPTO_USE_LTO=ON
ninja -j$(nproc)

# 4. Run
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 ./zepto_http_server --port 8123 --hugepages
```

---

## 1. CPU Pinning & Isolation

### Kernel Boot Parameters

Add to `/etc/default/grub` `GRUB_CMDLINE_LINUX`:

```
isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3
```

| Parameter | Purpose |
|-----------|---------|
| `isolcpus=0-3` | Remove cores 0-3 from general scheduler — dedicated to ZeptoDB |
| `nohz_full=0-3` | Disable timer ticks on isolated cores — eliminates jitter |
| `rcu_nocbs=0-3` | Offload RCU callbacks — no kernel interrupts on hot path |

After editing:
```bash
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

### Runtime CPU Affinity

ZeptoDB's `ResourceIsolation` supports `pthread_setaffinity_np` for per-thread pinning (implemented in `src/execution/resource_isolation.cpp`).

For manual control:
```bash
# Pin server to isolated cores
sudo numactl --cpunodebind=0 --membind=0 \
    taskset -c 0-3 ./zepto_http_server --port 8123

# Verify
taskset -p $(pidof zepto_http_server)
```

### IRQ Affinity

Move network interrupts away from ZeptoDB cores:

```bash
# Move NIC IRQs to system cores (8-15)
for irq in $(grep -E 'eth|mlx|ixgbe' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
    echo ff00 > /proc/irq/$irq/smp_affinity 2>/dev/null || true
done
```

### Multi-NUMA Layout

For dual-socket servers, run one instance per NUMA node:

```bash
# Node 0: realtime ingestion
numactl --cpunodebind=0 --membind=0 taskset -c 0-7 \
    ./zepto_http_server --port 8123 --node-id 0

# Node 1: analytics queries
numactl --cpunodebind=1 --membind=1 taskset -c 16-23 \
    ./zepto_http_server --port 8124 --node-id 1
```

Verify NUMA allocation:
```bash
numastat -p $(pidof zepto_http_server)
```

---

## 2. Hugepages

ZeptoDB's arena allocator uses `mmap` with `MAP_HUGETLB` for 2MB pages. Without hugepages, a 8.9GB working set requires ~2.2M TLB entries (4KB pages) vs ~4,448 entries (2MB pages) — a 512× difference in TLB pressure.

### Allocation

**Boot-time (recommended — avoids fragmentation):**
```
# /etc/default/grub
default_hugepagesz=2M hugepagesz=2M hugepages=16384
```

**Runtime:**
```bash
# Allocate 32GB of 2MB hugepages
echo 16384 > /proc/sys/vm/nr_hugepages

# Compact memory first if fragmented
echo 1 > /proc/sys/vm/drop_caches
echo 1 > /proc/sys/vm/compact_memory
```

**Persistent (sysctl):**
```bash
echo "vm.nr_hugepages = 16384" >> /etc/sysctl.d/99-zepto-tuning.conf
sysctl -p /etc/sysctl.d/99-zepto-tuning.conf
```

### Sizing Formula

```
hugepages = (num_partitions × arena_size_MB) / 2 + headroom_20%
```

Example: 278 partitions × 32MB = 8.9GB → `echo 4608 > /proc/sys/vm/nr_hugepages` (minimum)

### 1GB Hugepages (Advanced)

For very large working sets (>32GB), 1GB pages further reduce TLB misses:

```
# /etc/default/grub
hugepagesz=1G hugepages=32
```

> ⚠️ 1GB pages must be allocated at boot. Runtime allocation is not supported.

### Verification

```bash
grep -i huge /proc/meminfo
# HugePages_Total:    16384
# HugePages_Free:     12000   ← some used by ZeptoDB
# Hugepagesize:        2048 kB
```

---

## 3. C-State & CPU Frequency

### Disable Deep C-States

Deep sleep states (C2+) add wake-up latency (10-200μs). For consistent sub-microsecond response:

**Boot parameter (strongest):**
```
processor.max_cstate=1 intel_idle.max_cstate=0
```

**Runtime:**
```bash
for state in /sys/devices/system/cpu/cpu*/cpuidle/state[2-9]/disable; do
    echo 1 > "$state" 2>/dev/null || true
done
```

### CPU Governor

```bash
# Set all cores to performance (fixed max frequency)
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$gov" 2>/dev/null || true
done
```

> ⚠️ AWS EC2 instances virtualize CPU frequency controls — `scaling_governor` may not be writable. This is expected.

### Turbo Boost

Disable for latency consistency (turbo causes frequency transitions):

```bash
# Intel
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
# AMD
echo 0 > /sys/devices/system/cpu/cpufreq/boost
```

---

## 4. Kernel Sysctl Parameters

All parameters applied by `deploy/scripts/tune_bare_metal.sh`:

```bash
# Memory
vm.nr_hugepages         = 16384   # 32GB of 2MB pages (adjust to workload)
vm.swappiness           = 0       # never swap — in-memory DB
vm.numa_balancing       = 0       # ZeptoDB manages NUMA explicitly
vm.zone_reclaim_mode    = 0       # don't reclaim from local zone
vm.stat_interval        = 120     # reduce vmstat overhead
vm.dirty_ratio          = 80      # delay writeback (HDB flush is explicit)
vm.min_free_kbytes      = 262144  # 256MB free reserve for hugepage allocation
vm.vfs_cache_pressure   = 50      # keep dentries/inodes longer

# Scheduler
kernel.watchdog         = 0       # disable soft lockup detector
kernel.nmi_watchdog     = 0       # disable NMI watchdog
kernel.timer_migration  = 0       # don't migrate timers across CPUs

# Security (keep enabled)
kernel.randomize_va_space = 2     # ASLR ON — disabling hurts L3 cache aliasing
```

> ⚠️ `randomize_va_space=0` was tested and caused +4ms regression on Xbar due to L3 cache set aliasing from deterministic virtual addresses. Keep ASLR enabled.

### Persistent Configuration

```bash
sudo cp /etc/sysctl.d/99-zepto-tuning.conf /etc/sysctl.d/99-zepto-tuning.conf.bak
sudo ./deploy/scripts/tune_bare_metal.sh   # writes 99-zepto-tuning.conf
```

---

## 5. Network Tuning

```bash
# Low-latency polling
sysctl -w net.core.busy_poll=50          # busy-poll 50μs before sleeping
sysctl -w net.core.busy_read=50          # busy-read 50μs
sysctl -w net.ipv4.tcp_low_latency=1     # prefer latency over throughput

# Buffer sizes (128MB)
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728
sysctl -w net.ipv4.tcp_rmem="4096 87380 134217728"
sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"

# Backlog
sysctl -w net.core.netdev_max_backlog=10000
```

### Disable THP

Transparent Huge Pages cause unpredictable latency spikes from background compaction:

```bash
echo never > /sys/kernel/mm/transparent_hugepage/enabled
echo never > /sys/kernel/mm/transparent_hugepage/defrag
```

Or via boot parameter: `transparent_hugepage=never`

---

## 6. Build Optimization

### CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `ZEPTO_USE_TCMALLOC` | OFF | tcmalloc_minimal — 12% faster for small allocations |
| `ZEPTO_USE_LTO` | OFF | Link-Time Optimization — 8% cross-TU inlining gains |

### Recommended Production Build

```bash
cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DZEPTO_USE_TCMALLOC=ON \
    -DZEPTO_USE_LTO=ON

ninja -j$(nproc)
```

### PGO (Profile-Guided Optimization)

PGO provides additional 2-5% by optimizing branch prediction and inlining based on real workload profiles.

```bash
# Step 1: Instrumented build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=native -fprofile-generate=/tmp/zepto_pgo"
ninja -j$(nproc)

# Step 2: Run representative workload
./zepto_tests --gtest_filter="Benchmark.*:SqlExecutor*:FinancialFunction*:WindowJoin*"

# Step 3: Optimized build using profile
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DZEPTO_USE_TCMALLOC=ON -DZEPTO_USE_LTO=ON \
    -DCMAKE_CXX_FLAGS="-O3 -march=native -flto -fprofile-use=/tmp/zepto_pgo -fprofile-correction"
ninja -j$(nproc)
```

### Allocator Comparison

| Allocator | Xbar 1M | Improvement |
|-----------|---------|-------------|
| glibc malloc | 53.2ms | baseline |
| jemalloc | 51.6ms | +3% |
| tcmalloc_minimal | 47.5ms | +12% |

tcmalloc's per-thread caches handle ZeptoDB's pattern of many short-lived small allocations far better than glibc.

---

## 7. Benchmarking & Verification

### Run Benchmarks

```bash
cd build
ninja zepto_tests
./zepto_tests --gtest_filter="Benchmark*"
```

### Expected Results (Tuned)

| Benchmark | Untuned | Tuned (LTO+PGO+tcmalloc+hugepages) | Improvement |
|-----------|---------|-------------------------------------|-------------|
| Xbar 1M rows | 45.2ms | 12.4ms | -73% |
| EMA 1M rows | 2.15ms | 2.23ms | flat |
| Window JOIN 100K² | ~12ms | 11.6ms | flat |

### Profiling

```bash
# perf stat — check IPC (should be > 1.0 after tuning)
sudo perf stat -d ./zepto_tests --gtest_filter="Benchmark.Xbar*"

# Flamegraph
sudo perf record -F 999 -a -g -- sleep 30
sudo perf script | flamegraph.pl > zepto_flame.svg

# NUMA memory stats
numastat -p $(pidof zepto_http_server)

# Hugepage usage
grep Huge /proc/meminfo
```

### Verification Checklist

```bash
# CPU governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor  # → performance

# Hugepages allocated
cat /proc/sys/vm/nr_hugepages  # → 16384

# NUMA balancing off
cat /proc/sys/kernel/numa_balancing  # → 0

# THP disabled
cat /sys/kernel/mm/transparent_hugepage/enabled  # → [never]

# Isolated cores
cat /proc/cmdline | grep -o 'isolcpus=[^ ]*'  # → isolcpus=0-3

# C-states
cat /sys/devices/system/cpu/cpu0/cpuidle/state2/disable  # → 1
```

---

## 8. Automation

### tune_bare_metal.sh

The script at `deploy/scripts/tune_bare_metal.sh` applies all runtime tuning in one step:

```bash
sudo ./deploy/scripts/tune_bare_metal.sh
```

It handles: CPU governor, turbo boost, hugepages, IRQ affinity, network stack, NUMA balancing, swappiness, THP, C-states, and persists settings to `/etc/sysctl.d/99-zepto-tuning.conf`.

### AI Tuner

For iterative profiling and optimization, use the AI-driven tuner:

```bash
python3 deploy/scripts/ai_tune_bare_metal.py
```

This uses Claude Opus with extended thinking to profile the system, run benchmarks, and apply tuning commands iteratively. See [devlog/015](../devlog/015_bare_metal_tuning.md) for detailed results.

### systemd Service

```bash
sudo ./deploy/scripts/install_service.sh
# Installs zeptodb.service with NUMA/CPU affinity pre-configured
```

---

## Not Yet Implemented

| Feature | Status | Notes |
|---------|--------|-------|
| io_uring for HDB I/O | ❌ Not implemented | No `io_uring` code in codebase. HDB flush uses synchronous `write()`. Would benefit WAL writes and HDB flush on NVMe. |
| 1GB hugepage support | ❌ Untested | Arena allocator uses 2MB pages only. Kernel boot param works but ZeptoDB doesn't request 1GB pages via `MAP_HUGE_1GB`. |
| Clang ThinLTO | ⚠️ Blocked | Requires matching `lld` version. GCC `-flto` works. Clang 19 + lld-19 needed (`sudo yum install lld19`). |
| `isolcpus` in-code detection | ❌ Not implemented | Server doesn't verify it's running on isolated cores. Manual `taskset` required. |

---

## Reference

- [Devlog 015 — Bare Metal Tuning Results](../devlog/015_bare_metal_tuning.md)
- [Production Deployment Guide](PRODUCTION_DEPLOYMENT.md)
- [Cloud Performance Tuning](CLOUD_PERFORMANCE_TUNING.md)
- [tune_bare_metal.sh](../../deploy/scripts/tune_bare_metal.sh)
- [ai_tune_bare_metal.py](../../deploy/scripts/ai_tune_bare_metal.py)

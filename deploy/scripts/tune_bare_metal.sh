#!/bin/bash
# ZeptoDB 베어메탈 최적화 스크립트
# 실행: sudo ./tune_bare_metal.sh

set -e

echo "=== ZeptoDB Bare Metal Tuning Script ==="
echo "Version: 1.0.0"
echo "Date: $(date)"
echo ""

# Root 권한 확인
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)"
    exit 1
fi

# 1. CPU Governor: performance 모드
echo "[1/10] Setting CPU governor to performance..."
governor_count=0
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [ -f "$cpu" ]; then
        echo performance > "$cpu"
        ((governor_count++))
    fi
done
echo "  ✓ Set $governor_count CPUs to performance mode"

# 2. Turbo Boost 비활성화 (일관된 레이턴시)
echo "[2/10] Disabling Turbo Boost..."
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "  ✓ Intel Turbo Boost disabled"
elif [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    echo "  ✓ AMD Boost disabled"
else
    echo "  ⚠ Turbo Boost control not found (OK on some systems)"
fi

# 3. Hugepages: 32GB (16,384 x 2MB pages)
echo "[3/10] Configuring Hugepages (32GB = 16,384 x 2MB)..."
echo 16384 > /proc/sys/vm/nr_hugepages
allocated=$(cat /proc/sys/vm/nr_hugepages)
if [ "$allocated" -ge 16384 ]; then
    echo "  ✓ Hugepages allocated: $allocated pages ($(($allocated * 2))MB)"
else
    echo "  ⚠ WARNING: Only $allocated hugepages allocated (requested 16384)"
    echo "     This may be due to memory fragmentation. Reboot and retry."
fi

# 4. IRQ Affinity: 인터럽트를 시스템 코어(8-15)로
echo "[4/10] Setting IRQ affinity to cores 8-15..."
irq_count=0
for irq in $(grep -E 'eth|mlx|ixgbe' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
    if [ -f /proc/irq/$irq/smp_affinity ]; then
        echo ff00 > /proc/irq/$irq/smp_affinity 2>/dev/null && ((irq_count++)) || true
    fi
done
if [ $irq_count -gt 0 ]; then
    echo "  ✓ Set affinity for $irq_count network IRQs"
else
    echo "  ⚠ No network IRQs found (OK if no network device)"
fi

# 5. Network stack tuning (저지연)
echo "[5/10] Tuning network stack for low latency..."
sysctl -w net.core.busy_poll=50 > /dev/null
sysctl -w net.core.busy_read=50 > /dev/null
sysctl -w net.ipv4.tcp_low_latency=1 > /dev/null
sysctl -w net.core.netdev_max_backlog=10000 > /dev/null
sysctl -w net.core.rmem_max=134217728 > /dev/null
sysctl -w net.core.wmem_max=134217728 > /dev/null
sysctl -w net.ipv4.tcp_rmem="4096 87380 134217728" > /dev/null
sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728" > /dev/null
echo "  ✓ Network stack tuned"

# 6. Disable NUMA balancing (명시적 제어)
echo "[6/10] Disabling automatic NUMA balancing..."
echo 0 > /proc/sys/kernel/numa_balancing
echo "  ✓ NUMA balancing disabled"

# 7. Swappiness 최소화
echo "[7/10] Setting swappiness to 0..."
sysctl -w vm.swappiness=0 > /dev/null
echo "  ✓ Swappiness set to 0"

# 8. Transparent Huge Pages (THP) 비활성화
echo "[8/10] Disabling Transparent Huge Pages..."
if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled
    echo never > /sys/kernel/mm/transparent_hugepage/defrag
    echo "  ✓ THP disabled"
else
    echo "  ⚠ THP control not found (may be disabled by kernel param)"
fi

# 9. C-state 제한 (일관된 레이턴시)
echo "[9/10] Limiting CPU C-states..."
cstate_count=0
for state in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do
    if [ -f "$state" ] && [[ "$state" =~ state[2-9] ]]; then
        echo 1 > "$state" 2>/dev/null && ((cstate_count++)) || true
    fi
done
if [ $cstate_count -gt 0 ]; then
    echo "  ✓ Disabled $cstate_count deep C-states"
else
    echo "  ⚠ C-state control not found (may be disabled by kernel param)"
fi

# 10. 설정 영구화 (sysctl.conf)
echo "[10/10] Making network settings persistent..."
cat > /etc/sysctl.d/99-zepto-tuning.conf <<EOF
# ZeptoDB Bare Metal Tuning
net.core.busy_poll=50
net.core.busy_read=50
net.ipv4.tcp_low_latency=1
net.core.netdev_max_backlog=10000
net.core.rmem_max=134217728
net.core.wmem_max=134217728
net.ipv4.tcp_rmem=4096 87380 134217728
net.ipv4.tcp_wmem=4096 65536 134217728
kernel.numa_balancing=0
vm.swappiness=0
EOF
echo "  ✓ Settings saved to /etc/sysctl.d/99-zepto-tuning.conf"

echo ""
echo "=== Tuning Complete ==="
echo ""
echo "📊 Verification Commands:"
echo "  - CPU governor:        cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
echo "  - Hugepages:           cat /proc/meminfo | grep -i huge"
echo "  - NUMA nodes:          numactl --hardware"
echo "  - Kernel cmdline:      cat /proc/cmdline"
echo "  - Network tuning:      sysctl net.core.busy_poll"
echo ""
echo "⚠️  IMPORTANT: Verify kernel parameters in /etc/default/grub:"
echo "   isolcpus=0-3 nohz_full=0-3 rcu_nocbs=0-3"
echo "   If not set, add them and run: grub2-mkconfig -o /boot/grub2/grub.cfg && reboot"
echo ""
echo "🚀 Ready to run ZeptoDB with:"
echo "   numactl --cpunodebind=0 --membind=0 taskset -c 0-3 ./zepto_server --hugepages"

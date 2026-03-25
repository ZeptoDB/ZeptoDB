// ============================================================================
// Layer 1: Arena Allocator Implementation
// ============================================================================

#include "zeptodb/storage/arena_allocator.h"

#include <sys/mman.h>    // mmap, munmap
#ifdef __linux__
#include <numa.h>        // numa_alloc_onnode (Linux only)
#endif
#include <cerrno>
#include <cstring>
#include <stdexcept>

// macOS lacks MAP_HUGETLB and MAP_POPULATE
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

namespace zeptodb::storage {

ArenaAllocator::ArenaAllocator(const ArenaConfig& config)
    : total_size_(config.total_size)
    , hugepages_(config.use_hugepages)
{
    // Round up to HugePage boundary if enabled
    if (hugepages_) {
        total_size_ = align_up(total_size_, HUGEPAGE_SIZE_2M);
    }

    // 1차 시도: HugePages mmap (커널 바이패스, TLB miss 최소화)
    if (hugepages_) {
        base_ = ::mmap(
            nullptr,
            total_size_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
            -1, 0
        );
        if (base_ == MAP_FAILED) {
            ZEPTO_WARN("HugePages mmap failed (errno={}), falling back to regular pages",
                      std::strerror(errno));
            hugepages_ = false;
            base_ = nullptr;
        }
    }

#ifdef __linux__
    // 2차 시도: NUMA-aware 일반 할당 (Linux only)
    if (!base_ && config.numa_node >= 0 && numa_available() >= 0) {
        base_ = numa_alloc_onnode(total_size_, config.numa_node);
        if (!base_) {
            ZEPTO_WARN("NUMA alloc on node {} failed, falling back to regular mmap",
                      config.numa_node);
        }
    }
#endif

    // 3차 시도: 일반 mmap (MAP_POPULATE로 사전 폴트)
    if (!base_) {
        base_ = ::mmap(
            nullptr,
            total_size_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
            -1, 0
        );
        if (base_ == MAP_FAILED) {
            throw std::runtime_error(
                fmt::format("ArenaAllocator: mmap failed for {} bytes: {}",
                            total_size_, std::strerror(errno)));
        }
    }

    // Prefault: 전 영역 zero 터치로 page fault 사전 해소
    std::memset(base_, 0, total_size_);

    ZEPTO_INFO("Arena allocated: {} MB (hugepages={}, addr={})",
              total_size_ / (1024 * 1024), hugepages_, base_);
}

ArenaAllocator::~ArenaAllocator() {
    if (base_) {
        if (hugepages_) {
            ::munmap(base_, total_size_);
        } else {
            // Could be numa_alloc'd or mmap'd
            // Both can be munmap'd safely
            ::munmap(base_, total_size_);
        }
        ZEPTO_DEBUG("Arena freed: {} MB", total_size_ / (1024 * 1024));
    }
}

void* ArenaAllocator::allocate(size_t size, size_t alignment) {
    // Lock-free bump allocation via atomic fetch_add
    // CAS loop to handle alignment padding
    size_t current = offset_.load(std::memory_order_relaxed);
    size_t aligned_offset;
    size_t new_offset;

    do {
        aligned_offset = align_up(current, alignment);
        new_offset = aligned_offset + size;

        if (new_offset > total_size_) {
            ZEPTO_ERROR("Arena exhausted: requested {} bytes, only {} free",
                       size, total_size_ - current);
            return nullptr;
        }
    } while (!offset_.compare_exchange_weak(
        current, new_offset,
        std::memory_order_acq_rel,
        std::memory_order_relaxed
    ));

    return static_cast<char*>(base_) + aligned_offset;
}

void ArenaAllocator::reset() {
    offset_.store(0, std::memory_order_release);
    ZEPTO_DEBUG("Arena reset (capacity={} MB)", total_size_ / (1024 * 1024));
}

} // namespace zeptodb::storage

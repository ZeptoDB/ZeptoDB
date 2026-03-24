#pragma once
// ============================================================================
// Layer 1: Arena Allocator — Lock-Free Bump Pointer Allocator
// ============================================================================
// 문서 근거: layer1_storage_memory.md §4 "메모리 아레나(Arena) 기법"
//
// 설계 원칙:
//   - 거대한 메모리 풀을 HugePages로 사전 할당 (Arena)
//   - malloc/new 호출 ZERO — atomic bump pointer로 O(1) 할당
//   - Cache-line aligned 보장
//   - OS syscall 없음 (Kernel Bypass)
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/common/logger.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace zeptodb::storage {

// ============================================================================
// ArenaConfig: 아레나 설정
// ============================================================================
struct ArenaConfig {
    size_t total_size    = DEFAULT_ARENA_SIZE;   // 기본 256MB
    bool   use_hugepages = true;                 // HugePages 2MB 사용 여부
    int    numa_node     = -1;                   // NUMA 노드 (-1 = 자동)
};

// ============================================================================
// ArenaAllocator: Lock-Free Bump Pointer Allocator
// ============================================================================
class ArenaAllocator {
public:
    explicit ArenaAllocator(const ArenaConfig& config = {});
    ~ArenaAllocator();

    // Non-copyable, non-movable (메모리 풀 소유권)
    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    /// Lock-free 할당. alignment는 기본 CACHE_LINE_SIZE (64 bytes)
    /// @return nullptr if arena exhausted
    [[nodiscard]] void* allocate(size_t size, size_t alignment = CACHE_LINE_SIZE);

    /// 타입별 편의 할당
    template <typename T>
    [[nodiscard]] T* allocate_array(size_t count) {
        size_t alloc_size = sizeof(T) * count;
        size_t align = std::max(alignof(T), CACHE_LINE_SIZE);
        return static_cast<T*>(allocate(alloc_size, align));
    }

    /// 전체 아레나 리셋 (파티션 플러시 후 재사용)
    void reset();

    // --- Stats ---
    [[nodiscard]] size_t total_capacity() const { return total_size_; }
    [[nodiscard]] size_t used_bytes() const { return offset_.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t free_bytes() const { return total_size_ - used_bytes(); }
    [[nodiscard]] double utilization() const {
        return static_cast<double>(used_bytes()) / static_cast<double>(total_size_);
    }
    [[nodiscard]] void* base_ptr() const { return base_; }

private:
    void*                base_       = nullptr;  // 아레나 시작 주소
    size_t               total_size_ = 0;        // 전체 크기
    std::atomic<size_t>  offset_{0};             // Bump pointer (atomic)
    bool                 hugepages_  = false;     // HugePages 사용 여부
};

} // namespace zeptodb::storage

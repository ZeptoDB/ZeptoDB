#pragma once
// ============================================================================
// Layer 2: Lock-Free MPMC Ring Buffer
// ============================================================================
// 문서 근거: layer2_ingestion_network.md §4 "Ring Buffer 기반 Lock-Free Queue"
//   - Atomic Fetch-and-Add 기반 위치 선점
//   - CPU Pinning + Spin 폴링
//   - Zero context-switch
// ============================================================================

#include "zeptodb/common/types.h"
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>

namespace zeptodb::ingestion {

/// Ring Buffer 슬롯 상태
enum class SlotState : uint32_t {
    EMPTY = 0,
    WRITING = 1,
    READY = 2,
    READING = 3,
};

/// 캐시라인 패딩된 슬롯
template <typename T>
struct alignas(CACHE_LINE_SIZE) Slot {
    std::atomic<SlotState> state{SlotState::EMPTY};
    T data;
};

// ============================================================================
// MPMCRingBuffer: Lock-Free Multi-Producer Multi-Consumer Queue
// ============================================================================
template <typename T, size_t Capacity = 65536>
class MPMCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    MPMCRingBuffer() : slots_(new Slot<T>[Capacity]) {}
    ~MPMCRingBuffer() { delete[] slots_; }

    // Non-copyable, movable
    MPMCRingBuffer(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;

    /// 생산자: 데이터 enqueue (non-blocking)
    /// @return true if successfully enqueued
    bool try_push(const T& item) {
        size_t pos = write_pos_.fetch_add(1, std::memory_order_relaxed);
        size_t idx = pos & (Capacity - 1);
        auto& slot = slots_[idx];

        // 슬롯이 비어있는지 확인 (spin이 아닌 1회 체크)
        SlotState expected = SlotState::EMPTY;
        if (!slot.state.compare_exchange_strong(
                expected, SlotState::WRITING,
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            // Ring이 꽉 찬 상태 — 생산자가 소비자를 추월
            return false;
        }

        slot.data = item;
        slot.state.store(SlotState::READY, std::memory_order_release);
        return true;
    }

    /// 소비자: 데이터 dequeue (non-blocking)
    /// @return 데이터 또는 nullopt
    std::optional<T> try_pop() {
        size_t pos = read_pos_.load(std::memory_order_relaxed);
        size_t idx = pos & (Capacity - 1);
        auto& slot = slots_[idx];

        // Check slot state BEFORE advancing read_pos_.
        // The original fetch_add-first design advances read_pos_ even on an empty
        // queue, which causes the consumer to permanently skip slots that are
        // written later (producer writes to slot N, but read_pos_ is already > N).
        SlotState expected = SlotState::READY;
        if (!slot.state.compare_exchange_strong(
                expected, SlotState::READING,
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            // Slot not ready (empty queue or another consumer already claimed it).
            return std::nullopt;
        }

        // Slot claimed — now advance read_pos_ and read the data.
        read_pos_.fetch_add(1, std::memory_order_relaxed);
        T item = slot.data;
        slot.state.store(SlotState::EMPTY, std::memory_order_release);
        return item;
    }

    /// 현재 대략적인 큐 크기 (정확하지 않음 — 근사치)
    [[nodiscard]] size_t approx_size() const {
        auto w = write_pos_.load(std::memory_order_relaxed);
        auto r = read_pos_.load(std::memory_order_relaxed);
        return w >= r ? w - r : 0;
    }

    [[nodiscard]] static constexpr size_t capacity() { return Capacity; }

private:
    // 캐시라인 분리로 false sharing 방지
    ZEPTO_CACHE_ALIGNED std::atomic<size_t> write_pos_{0};
    ZEPTO_CACHE_ALIGNED std::atomic<size_t> read_pos_{0};
    Slot<T>* slots_;
};

} // namespace zeptodb::ingestion

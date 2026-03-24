// ============================================================================
// ZeptoDB: Optimized FIX Parser (Zero-Copy + SIMD)
// ============================================================================
#pragma once

#include "zeptodb/feeds/fix_parser.h"
#include <immintrin.h>  // AVX2/AVX-512

namespace zeptodb::feeds::optimized {

// ============================================================================
// Fast FIX Parser (최적화 버전)
// ============================================================================
class FIXParserFast : public FIXParser {
public:
    FIXParserFast();

    // 배치 파싱 (여러 메시지 한번에)
    size_t parse_batch(const char* buffer, size_t len,
                       Tick* ticks, size_t max_ticks,
                       SymbolMapper* mapper);

    // Zero-copy 파싱 (필드 복사 없음)
    bool parse_zero_copy(const char* msg, size_t len);

    // 필드 조회 (zero-copy, string_view 반환)
    bool get_field_view(int tag, const char*& value, size_t& len) const;

private:
    // 필드 오프셋 캐시 (복사 대신 포인터)
    struct FieldView {
        const char* ptr;
        size_t len;
    };
    FieldView field_views_[256];  // tag → (ptr, len)

    // SIMD 최적화 SOH 검색
    const char* find_soh_simd(const char* start, const char* end) const;

    // 빠른 정수 파싱 (atoi보다 빠름)
    int64_t parse_int_fast(const char* str, size_t len) const;

    // 빠른 실수 파싱
    double parse_double_fast(const char* str, size_t len) const;
};

// ============================================================================
// Memory Pool for Ticks (메모리 할당 최적화)
// ============================================================================
class TickMemoryPool {
public:
    explicit TickMemoryPool(size_t pool_size = 100000);
    ~TickMemoryPool();

    // Tick 할당
    Tick* allocate();

    // 배치 할당
    Tick* allocate_batch(size_t count);

    // 풀 초기화 (재사용)
    void reset();

    // 통계
    size_t get_allocated_count() const { return allocated_; }
    size_t get_capacity() const { return capacity_; }

private:
    Tick* pool_;
    size_t capacity_;
    size_t allocated_;
};

// ============================================================================
// Lock-free Ring Buffer (멀티스레드 최적화)
// ============================================================================
template<typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t capacity);
    ~LockFreeRingBuffer();

    // 삽입 (생산자)
    bool push(const T& item);

    // 추출 (소비자)
    bool pop(T& item);

    // 상태
    bool is_empty() const;
    bool is_full() const;
    size_t size() const;

private:
    T* buffer_;
    size_t capacity_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;

    // Cache line padding (false sharing 방지)
    alignas(64) std::atomic<size_t> head_cache_;
    alignas(64) std::atomic<size_t> tail_cache_;
};

// ============================================================================
// SIMD Helpers
// ============================================================================
#ifdef __AVX2__
// AVX2로 SOH (0x01) 검색
inline const char* find_soh_avx2(const char* start, const char* end) {
    const char SOH = 0x01;
    const char* ptr = start;

    // 32바이트씩 검색
    while (ptr + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
        __m256i soh_vec = _mm256_set1_epi8(SOH);
        __m256i cmp = _mm256_cmpeq_epi8(chunk, soh_vec);

        int mask = _mm256_movemask_epi8(cmp);
        if (mask != 0) {
            return ptr + __builtin_ctz(mask);
        }

        ptr += 32;
    }

    // 나머지 스칼라 검색
    while (ptr < end) {
        if (*ptr == SOH) return ptr;
        ++ptr;
    }

    return nullptr;
}
#endif

// ============================================================================
// Template Implementation
// ============================================================================
template<typename T>
LockFreeRingBuffer<T>::LockFreeRingBuffer(size_t capacity)
    : capacity_(capacity)
    , head_(0)
    , tail_(0)
    , head_cache_(0)
    , tail_cache_(0)
{
    buffer_ = new T[capacity];
}

template<typename T>
LockFreeRingBuffer<T>::~LockFreeRingBuffer() {
    delete[] buffer_;
}

template<typename T>
bool LockFreeRingBuffer<T>::push(const T& item) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t next_head = (head + 1) % capacity_;

    if (next_head == tail_.load(std::memory_order_acquire)) {
        return false;  // Full
    }

    buffer_[head] = item;
    head_.store(next_head, std::memory_order_release);
    return true;
}

template<typename T>
bool LockFreeRingBuffer<T>::pop(T& item) {
    size_t tail = tail_.load(std::memory_order_relaxed);

    if (tail == head_.load(std::memory_order_acquire)) {
        return false;  // Empty
    }

    item = buffer_[tail];
    tail_.store((tail + 1) % capacity_, std::memory_order_release);
    return true;
}

template<typename T>
bool LockFreeRingBuffer<T>::is_empty() const {
    return tail_.load(std::memory_order_acquire) ==
           head_.load(std::memory_order_acquire);
}

template<typename T>
bool LockFreeRingBuffer<T>::is_full() const {
    size_t head = head_.load(std::memory_order_acquire);
    size_t next_head = (head + 1) % capacity_;
    return next_head == tail_.load(std::memory_order_acquire);
}

template<typename T>
size_t LockFreeRingBuffer<T>::size() const {
    size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_acquire);

    if (head >= tail) {
        return head - tail;
    } else {
        return capacity_ - tail + head;
    }
}

} // namespace zeptodb::feeds::optimized

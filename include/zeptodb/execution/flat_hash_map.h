#pragma once
// ============================================================================
// FlatHashMap: Open-addressing hash map for join operators
// ============================================================================
// CRC32 intrinsic hash + linear probing + power-of-2 capacity.
// Specialized for int64_t keys with small vector values (index lists).
//
// Why not std::unordered_map:
//   - Node-based → pointer chasing → cache misses on every lookup
//   - FlatHashMap stores entries inline → sequential memory access
//   - CRC32 hardware hash → single instruction vs multi-cycle hash
//
// Design:
//   - Open addressing with linear probing (cache-friendly)
//   - Tombstone-free (no erase support — join maps are build-once)
//   - Load factor ≤ 0.75 → expected probe length < 2
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

namespace zeptodb::execution {

// ============================================================================
// Hardware-accelerated hash for int64_t keys
// ============================================================================
inline uint64_t hash_i64(int64_t key) {
#ifdef __SSE4_2__
    // CRC32: single-cycle on Intel/AMD with SSE4.2
    uint64_t h = _mm_crc32_u64(0, static_cast<uint64_t>(key));
    return h;
#else
    // Fallback: splitmix64 finalizer
    uint64_t x = static_cast<uint64_t>(key);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
#endif
}

// ============================================================================
// FlatHashMap<V>: int64_t key → V value, open-addressing
// ============================================================================
template <typename V>
class FlatHashMap {
public:
    explicit FlatHashMap(size_t expected_entries = 16) {
        // Round up to power of 2, with load factor headroom
        size_t cap = 16;
        size_t target = expected_entries * 4 / 3 + 1; // ~75% load factor
        while (cap < target) cap <<= 1;
        mask_ = cap - 1;
        entries_.resize(cap);
    }

    // Insert or access entry for key (like operator[] on std::unordered_map)
    V& operator[](int64_t key) {
        size_t idx = hash_i64(key) & mask_;
        while (true) {
            auto& e = entries_[idx];
            if (!e.occupied) {
                e.occupied = true;
                e.key = key;
                ++size_;
                return e.value;
            }
            if (e.key == key) {
                return e.value;
            }
            idx = (idx + 1) & mask_;
        }
    }

    // Find entry, return pointer to value or nullptr
    const V* find(int64_t key) const {
        size_t idx = hash_i64(key) & mask_;
        while (true) {
            const auto& e = entries_[idx];
            if (!e.occupied) return nullptr;
            if (e.key == key) return &e.value;
            idx = (idx + 1) & mask_;
        }
    }

    V* find(int64_t key) {
        size_t idx = hash_i64(key) & mask_;
        while (true) {
            auto& e = entries_[idx];
            if (!e.occupied) return nullptr;
            if (e.key == key) return &e.value;
            idx = (idx + 1) & mask_;
        }
    }

    [[nodiscard]] size_t size() const { return size_; }

    // Iterate all occupied entries: fn(int64_t key, V& value)
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (auto& e : entries_) {
            if (e.occupied) fn(e.key, e.value);
        }
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& e : entries_) {
            if (e.occupied) fn(e.key, e.value);
        }
    }

private:
    struct Entry {
        bool    occupied = false;
        int64_t key      = 0;
        V       value{};
    };

    std::vector<Entry> entries_;
    size_t mask_ = 0;
    size_t size_ = 0;
};

} // namespace zeptodb::execution

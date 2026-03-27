#pragma once
// ============================================================================
// ZeptoDB Common Types & Constants
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace zeptodb {

// ============================================================================
// Fundamental Types (금융 데이터 최적화)
// ============================================================================
using Timestamp = int64_t;      // Nanoseconds since epoch
using SymbolId  = uint32_t;     // Interned symbol identifier
using SeqNum    = uint64_t;     // Global sequence number (FIFO ordering)
using Price     = int64_t;      // Fixed-point price (x10000, 소수점 4자리)
using Volume    = int64_t;      // Volume (signed for delta support)

// ============================================================================
// Memory Constants
// ============================================================================
constexpr size_t CACHE_LINE_SIZE   = 64;
constexpr size_t HUGEPAGE_SIZE_2M  = 2ULL * 1024 * 1024;
constexpr size_t HUGEPAGE_SIZE_1G  = 1ULL * 1024 * 1024 * 1024;
constexpr size_t DEFAULT_ARENA_SIZE = 256ULL * 1024 * 1024; // 256 MB

// DataBlock size for vectorized execution (Layer 3)
constexpr size_t DATABLOCK_ROWS = 8192;

// ============================================================================
// Alignment helpers
// ============================================================================
template <typename T>
constexpr T align_up(T value, T alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Cache-line aligned attribute
#define ZEPTO_CACHE_ALIGNED alignas(zeptodb::CACHE_LINE_SIZE)

} // namespace zeptodb

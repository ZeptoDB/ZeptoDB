#pragma once
// ============================================================================
// Layer 1: Column Store — Apache Arrow 호환 컬럼형 스토리지
// ============================================================================
// 문서 근거: layer1_storage_memory.md §3 "Columnar 연속성 보장"
//            architecture_design.md §1-A "Columnar Layout"
//
// 설계 원칙:
//   - Arrow C Data Interface 호환 레이아웃
//   - Cache-line aligned 연속 배열
//   - 포인터 체이싱 ZERO
//   - Arena Allocator에서 할당 (malloc 없음)
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/storage/arena_allocator.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace zeptodb::storage {

// ============================================================================
// ColumnType: 지원되는 컬럼 데이터 타입
// ============================================================================
enum class ColumnType : uint8_t {
    INT32,
    INT64,
    FLOAT32,
    FLOAT64,
    TIMESTAMP_NS,   // int64 nanosecond epoch
    SYMBOL,          // uint32 interned string id
    BOOL,
    STRING,          // dictionary-encoded, stored as uint32 codes
};

/// 타입별 바이트 크기
constexpr size_t column_type_size(ColumnType type) {
    switch (type) {
        case ColumnType::INT32:         return 4;
        case ColumnType::INT64:         return 8;
        case ColumnType::FLOAT32:       return 4;
        case ColumnType::FLOAT64:       return 8;
        case ColumnType::TIMESTAMP_NS:  return 8;
        case ColumnType::SYMBOL:        return 4;
        case ColumnType::BOOL:          return 1;
        case ColumnType::STRING:        return 4;  // dictionary code (uint32)
    }
    return 0;
}

// ============================================================================
// ColumnVector: 단일 컬럼 (Arrow-compatible flat buffer)
// ============================================================================
class ColumnVector {
public:
    ColumnVector(std::string name, ColumnType type, ArenaAllocator& arena);

    /// 현재 행 수
    [[nodiscard]] size_t size() const { return size_; }

    /// 컬럼 이름
    [[nodiscard]] const std::string& name() const { return name_; }

    /// 컬럼 타입
    [[nodiscard]] ColumnType type() const { return type_; }

    /// 원시 데이터 포인터 (Zero-copy 접근용)
    [[nodiscard]] void* raw_data() { return data_; }
    [[nodiscard]] const void* raw_data() const { return data_; }

    /// 타입별 span 접근 (type-safe, zero-copy)
    template <typename T>
    [[nodiscard]] std::span<T> as_span() {
        return std::span<T>(static_cast<T*>(data_), size_);
    }

    template <typename T>
    [[nodiscard]] std::span<const T> as_span() const {
        return std::span<const T>(static_cast<const T*>(data_), size_);
    }

    /// 값 추가 (Append-Only, 시계열)
    /// 내부적으로 아레나에서 용량 확장 처리
    template <typename T>
    bool append(T value);

    /// bulk append (SIMD-friendly batch insert)
    template <typename T>
    bool append_batch(const T* values, size_t count);

    /// 용량 (현재 할당된 최대 행 수)
    [[nodiscard]] size_t capacity() const { return capacity_; }

    /// Shrink logical size (for DELETE compaction). New size must be <= current size.
    void set_size(size_t new_size) { if (new_size <= size_) size_ = new_size; }

private:
    bool ensure_capacity(size_t needed);

    std::string       name_;
    ColumnType        type_;
    ArenaAllocator&   arena_;
    void*             data_     = nullptr;
    size_t            size_     = 0;
    size_t            capacity_ = 0;
    size_t            elem_size_ = 0;
};

// ============================================================================
// DataBlock: 다중 컬럼 묶음 (벡터화 실행 단위)
// ============================================================================
// Layer 3에서 8192 rows 단위로 처리하는 파이프라인의 기본 데이터 단위
struct DataBlock {
    std::vector<ColumnVector*> columns;
    size_t num_rows = 0;
};

} // namespace zeptodb::storage

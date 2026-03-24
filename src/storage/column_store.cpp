// ============================================================================
// Layer 1: Column Store Implementation
// ============================================================================

#include "zeptodb/storage/column_store.h"
#include <cstring>
#include <algorithm>

namespace zeptodb::storage {

// Initial capacity: 1 DataBlock worth of rows
static constexpr size_t INITIAL_CAPACITY = DATABLOCK_ROWS;
// Growth factor: double when full
static constexpr size_t GROWTH_FACTOR = 2;

ColumnVector::ColumnVector(std::string name, ColumnType type, ArenaAllocator& arena)
    : name_(std::move(name))
    , type_(type)
    , arena_(arena)
    , elem_size_(column_type_size(type))
{
    // Pre-allocate initial capacity from arena
    ensure_capacity(INITIAL_CAPACITY);
}

bool ColumnVector::ensure_capacity(size_t needed) {
    if (needed <= capacity_) return true;

    size_t new_capacity = capacity_ == 0
        ? std::max(needed, INITIAL_CAPACITY)
        : capacity_;

    while (new_capacity < needed) {
        new_capacity *= GROWTH_FACTOR;
    }

    // Allocate new block from arena (cache-line aligned)
    void* new_data = arena_.allocate(new_capacity * elem_size_, CACHE_LINE_SIZE);
    if (!new_data) {
        ZEPTO_ERROR("ColumnVector '{}': arena allocation failed for {} elements",
                   name_, new_capacity);
        return false;
    }

    // Copy existing data if any
    if (data_ && size_ > 0) {
        std::memcpy(new_data, data_, size_ * elem_size_);
    }

    // Note: old data stays in arena (arena doesn't support individual free)
    // This is by design — arena is reset as a whole on partition flush
    data_ = new_data;
    capacity_ = new_capacity;
    return true;
}

template <typename T>
bool ColumnVector::append(T value) {
    if (!ensure_capacity(size_ + 1)) return false;

    static_cast<T*>(data_)[size_] = value;
    ++size_;
    return true;
}

template <typename T>
bool ColumnVector::append_batch(const T* values, size_t count) {
    if (!ensure_capacity(size_ + count)) return false;

    std::memcpy(
        static_cast<char*>(data_) + (size_ * elem_size_),
        values,
        count * sizeof(T)
    );
    size_ += count;
    return true;
}

// Explicit template instantiations for supported types
template bool ColumnVector::append<int32_t>(int32_t);
template bool ColumnVector::append<int64_t>(int64_t);
template bool ColumnVector::append<float>(float);
template bool ColumnVector::append<double>(double);
template bool ColumnVector::append<uint32_t>(uint32_t);
template bool ColumnVector::append<uint8_t>(uint8_t);

template bool ColumnVector::append_batch<int32_t>(const int32_t*, size_t);
template bool ColumnVector::append_batch<int64_t>(const int64_t*, size_t);
template bool ColumnVector::append_batch<float>(const float*, size_t);
template bool ColumnVector::append_batch<double>(const double*, size_t);
template bool ColumnVector::append_batch<uint32_t>(const uint32_t*, size_t);
template bool ColumnVector::append_batch<uint8_t>(const uint8_t*, size_t);

} // namespace zeptodb::storage

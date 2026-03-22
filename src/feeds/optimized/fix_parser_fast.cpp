// ============================================================================
// APEX-DB: Optimized FIX Parser Implementation
// ============================================================================
#include "apex/feeds/optimized/fix_parser_fast.h"
#include <cstring>
#include <cstdlib>

namespace apex::feeds::optimized {

FIXParserFast::FIXParserFast() : FIXParser() {
    std::memset(field_views_, 0, sizeof(field_views_));
}

bool FIXParserFast::parse_zero_copy(const char* msg, size_t len) {
    std::memset(field_views_, 0, sizeof(field_views_));

    constexpr char SOH = 0x01;
    const char* ptr = msg;
    const char* end = msg + len;

    while (ptr < end) {
        // Tag 파싱 (빠른 정수 변환)
        int tag = 0;
        const char* tag_start = ptr;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            tag = tag * 10 + (*ptr - '0');
            ++ptr;
        }

        if (ptr >= end || *ptr != '=' || tag >= 256) {
            return false;
        }
        ++ptr; // skip '='

        // Value 파싱 (포인터만 저장)
        const char* value_start = ptr;

#ifdef __AVX2__
        // SIMD로 SOH 검색
        const char* soh_ptr = find_soh_avx2(ptr, end);
        if (soh_ptr) {
            ptr = soh_ptr;
        } else {
            ptr = end;
        }
#else
        // 스칼라 검색
        while (ptr < end && *ptr != SOH) {
            ++ptr;
        }
#endif

        // FieldView 저장 (zero-copy)
        field_views_[tag].ptr = value_start;
        field_views_[tag].len = ptr - value_start;

        // MsgType 저장
        if (tag == FIXTag::MsgType && field_views_[tag].len > 0) {
            msg_type_ = parse_msg_type(std::string(value_start, field_views_[tag].len));
        }

        if (ptr < end) ++ptr; // skip SOH
    }

    return true;
}

bool FIXParserFast::get_field_view(int tag, const char*& value, size_t& len) const {
    if (tag >= 256 || field_views_[tag].ptr == nullptr) {
        return false;
    }
    value = field_views_[tag].ptr;
    len = field_views_[tag].len;
    return true;
}

size_t FIXParserFast::parse_batch(const char* buffer, size_t len,
                                   Tick* ticks, size_t max_ticks,
                                   SymbolMapper* mapper) {
    size_t count = 0;
    const char* ptr = buffer;
    const char* end = buffer + len;

    while (ptr < end && count < max_ticks) {
        // 메시지 경계 찾기 (간단화: 10=로 구분)
        const char* msg_start = ptr;
        const char* msg_end = std::strstr(ptr, "\x01" "10=");
        if (!msg_end) break;

        // Checksum 끝까지 (3자리 + SOH)
        msg_end += 6;

        size_t msg_len = msg_end - msg_start;

        // 파싱
        if (parse_zero_copy(msg_start, msg_len)) {
            if (extract_tick(ticks[count], mapper)) {
                ++count;
            }
        }

        ptr = msg_end;
    }

    return count;
}

const char* FIXParserFast::find_soh_simd(const char* start, const char* end) const {
#ifdef __AVX2__
    return find_soh_avx2(start, end);
#else
    // Fallback to scalar
    while (start < end) {
        if (*start == 0x01) return start;
        ++start;
    }
    return nullptr;
#endif
}

int64_t FIXParserFast::parse_int_fast(const char* str, size_t len) const {
    int64_t result = 0;
    bool negative = false;
    size_t i = 0;

    if (len > 0 && str[0] == '-') {
        negative = true;
        i = 1;
    }

    for (; i < len && str[i] >= '0' && str[i] <= '9'; ++i) {
        result = result * 10 + (str[i] - '0');
    }

    return negative ? -result : result;
}

double FIXParserFast::parse_double_fast(const char* str, size_t len) const {
    // 간단한 구현 (strtod보다 빠름)
    double result = 0.0;
    bool negative = false;
    size_t i = 0;

    if (len > 0 && str[0] == '-') {
        negative = true;
        i = 1;
    }

    // 정수 부분
    for (; i < len && str[i] >= '0' && str[i] <= '9'; ++i) {
        result = result * 10.0 + (str[i] - '0');
    }

    // 소수점
    if (i < len && str[i] == '.') {
        ++i;
        double fraction = 0.1;
        for (; i < len && str[i] >= '0' && str[i] <= '9'; ++i) {
            result += (str[i] - '0') * fraction;
            fraction *= 0.1;
        }
    }

    return negative ? -result : result;
}

// ============================================================================
// TickMemoryPool
// ============================================================================
TickMemoryPool::TickMemoryPool(size_t pool_size)
    : capacity_(pool_size)
    , allocated_(0)
{
    pool_ = new Tick[capacity_];
}

TickMemoryPool::~TickMemoryPool() {
    delete[] pool_;
}

Tick* TickMemoryPool::allocate() {
    if (allocated_ >= capacity_) {
        return nullptr;
    }
    return &pool_[allocated_++];
}

Tick* TickMemoryPool::allocate_batch(size_t count) {
    if (allocated_ + count > capacity_) {
        return nullptr;
    }
    Tick* batch = &pool_[allocated_];
    allocated_ += count;
    return batch;
}

void TickMemoryPool::reset() {
    allocated_ = 0;
}

} // namespace apex::feeds::optimized

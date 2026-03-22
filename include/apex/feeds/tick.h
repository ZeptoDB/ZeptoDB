// ============================================================================
// APEX-DB: Feed Handler 공통 데이터 구조
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace apex::feeds {

// ============================================================================
// 틱 데이터 타입
// ============================================================================
enum class TickType : uint8_t {
    TRADE = 0,      // 체결
    QUOTE = 1,      // 호가
    ORDER = 2,      // 주문
    CANCEL = 3,     // 취소
    MODIFY = 4,     // 정정
    UNKNOWN = 255
};

// ============================================================================
// Side (매수/매도)
// ============================================================================
enum class Side : uint8_t {
    BUY = 0,
    SELL = 1,
    UNKNOWN = 255
};

// ============================================================================
// Tick 구조체 (zero-copy 최적화)
// ============================================================================
struct Tick {
    uint32_t symbol_id;          // 심볼 ID (interned)
    uint64_t timestamp_ns;       // 나노초 타임스탬프
    double   price;              // 가격
    uint64_t volume;             // 거래량
    TickType type;               // 틱 타입
    Side     side;               // 매수/매도
    uint64_t order_id;           // 주문 ID (옵션)
    uint32_t sequence;           // 시퀀스 번호 (옵션)

    Tick()
        : symbol_id(0)
        , timestamp_ns(0)
        , price(0.0)
        , volume(0)
        , type(TickType::UNKNOWN)
        , side(Side::UNKNOWN)
        , order_id(0)
        , sequence(0)
    {}
};

// ============================================================================
// Quote (호가) 구조체
// ============================================================================
struct Quote {
    uint32_t symbol_id;
    uint64_t timestamp_ns;
    double   bid_price;
    uint64_t bid_volume;
    double   ask_price;
    uint64_t ask_volume;
    uint32_t sequence;

    Quote()
        : symbol_id(0)
        , timestamp_ns(0)
        , bid_price(0.0)
        , bid_volume(0)
        , ask_price(0.0)
        , ask_volume(0)
        , sequence(0)
    {}
};

// ============================================================================
// Order (주문) 구조체
// ============================================================================
struct Order {
    uint32_t symbol_id;
    uint64_t timestamp_ns;
    uint64_t order_id;
    Side     side;
    double   price;
    uint64_t volume;
    uint32_t sequence;

    Order()
        : symbol_id(0)
        , timestamp_ns(0)
        , order_id(0)
        , side(Side::UNKNOWN)
        , price(0.0)
        , volume(0)
        , sequence(0)
    {}
};

// ============================================================================
// 심볼 조회 헬퍼 (symbol string → symbol_id)
// ============================================================================
class SymbolMapper {
public:
    virtual ~SymbolMapper() = default;
    virtual uint32_t get_symbol_id(const std::string& symbol) = 0;
    virtual std::string get_symbol_name(uint32_t symbol_id) = 0;
};

// ============================================================================
// 타임스탬프 유틸리티
// ============================================================================
inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

inline uint64_t micros_to_nanos(uint64_t micros) {
    return micros * 1000;
}

inline uint64_t millis_to_nanos(uint64_t millis) {
    return millis * 1000000;
}

} // namespace apex::feeds

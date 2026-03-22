// ============================================================================
// APEX-DB: NASDAQ ITCH Protocol Parser
// ============================================================================
// NASDAQ TotalView-ITCH 5.0 프로토콜
// Binary protocol, packed structures
// ============================================================================
#pragma once

#include "apex/feeds/tick.h"
#include <cstdint>

namespace apex::feeds {

// ============================================================================
// ITCH Message Types
// ============================================================================
enum class ITCHMessageType : uint8_t {
    SYSTEM_EVENT = 'S',
    STOCK_DIRECTORY = 'R',
    STOCK_TRADING_ACTION = 'H',
    REG_SHO_RESTRICTION = 'Y',
    MARKET_PARTICIPANT_POSITION = 'L',
    MWCB_DECLINE_LEVEL = 'V',
    MWCB_STATUS = 'W',
    IPO_QUOTING_PERIOD_UPDATE = 'K',
    ADD_ORDER = 'A',
    ADD_ORDER_MPID = 'F',
    ORDER_EXECUTED = 'E',
    ORDER_EXECUTED_WITH_PRICE = 'C',
    ORDER_CANCEL = 'X',
    ORDER_DELETE = 'D',
    ORDER_REPLACE = 'U',
    TRADE = 'P',
    CROSS_TRADE = 'Q',
    BROKEN_TRADE = 'B',
    NOII = 'I',
    RPII = 'N'
};

// ============================================================================
// ITCH Message Structures (packed, big-endian)
// ============================================================================
#pragma pack(push, 1)

struct ITCHMessageHeader {
    uint16_t length;    // 메시지 길이 (big-endian)
    uint8_t type;       // 메시지 타입
};

// Add Order (Type A)
struct ITCHAddOrder {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;         // 나노초
    uint64_t order_reference;
    char buy_sell_indicator;    // 'B' or 'S'
    uint32_t shares;
    char stock[8];              // 심볼 (공백 패딩)
    uint32_t price;             // 10000 단위 (e.g., 1500000 = $150.00)
};

// Order Executed (Type E)
struct ITCHOrderExecuted {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference;
    uint32_t executed_shares;
    uint64_t match_number;
};

// Trade (Type P) - Non-Cross
struct ITCHTrade {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price;
    uint64_t match_number;
};

#pragma pack(pop)

// ============================================================================
// NASDAQ ITCH Parser
// ============================================================================
class NASDAQITCHParser {
public:
    NASDAQITCHParser();
    ~NASDAQITCHParser() = default;

    // 패킷 파싱 (여러 메시지 포함 가능)
    bool parse_packet(const uint8_t* data, size_t len);

    // 현재 메시지에서 Tick 추출
    bool extract_tick(Tick& tick, SymbolMapper* mapper) const;

    // 메시지 타입
    ITCHMessageType get_message_type() const { return msg_type_; }

    // 통계
    uint64_t get_message_count() const { return message_count_; }
    uint64_t get_error_count() const { return error_count_; }

private:
    ITCHMessageType msg_type_;
    const uint8_t* current_message_;
    size_t current_message_len_;

    uint64_t message_count_;
    uint64_t error_count_;

    // 헬퍼
    uint16_t read_uint16_be(const uint8_t* ptr) const;
    uint32_t read_uint32_be(const uint8_t* ptr) const;
    uint64_t read_uint64_be(const uint8_t* ptr) const;
    std::string parse_stock_symbol(const char stock[8]) const;
    Side parse_buy_sell(char indicator) const;
    double parse_price(uint32_t price) const;
};

} // namespace apex::feeds

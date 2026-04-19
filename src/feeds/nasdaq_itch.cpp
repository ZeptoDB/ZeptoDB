// ============================================================================
// ZeptoDB: NASDAQ ITCH Parser Implementation
// ============================================================================
#include "zeptodb/feeds/nasdaq_itch.h"
#include <cstring>
#include <algorithm>

namespace zeptodb::feeds {

NASDAQITCHParser::NASDAQITCHParser()
    : msg_type_(ITCHMessageType::SYSTEM_EVENT)
    , current_message_(nullptr)
    , current_message_len_(0)
    , message_count_(0)
    , error_count_(0)
{}

bool NASDAQITCHParser::parse_packet(const uint8_t* data, size_t len) {
    if (len < sizeof(ITCHMessageHeader)) {
        ++error_count_;
        return false;
    }

    // ITCH 패킷은 여러 메시지를 포함할 수 있음
    // 간단화: 첫 메시지만 파싱
    ITCHMessageHeader header;
    std::memcpy(&header, data, sizeof(header));

    // Big-endian → Host byte order
    header.length = read_uint16_be(reinterpret_cast<const uint8_t*>(&header.length));

    msg_type_ = static_cast<ITCHMessageType>(header.type);
    current_message_ = data + sizeof(ITCHMessageHeader);
    current_message_len_ = len - sizeof(ITCHMessageHeader);

    ++message_count_;
    return true;
}

bool NASDAQITCHParser::extract_tick(Tick& tick, SymbolMapper* mapper) const {
    if (current_message_ == nullptr) return false;

    // devlog 086 (D3): auto-stamp table_id so downstream pipeline ingest routes
    // to the correct SchemaRegistry-assigned table. 0 = legacy/default.
    tick.table_id = table_id_;

    switch (msg_type_) {
        case ITCHMessageType::ADD_ORDER: {
            if (current_message_len_ < sizeof(ITCHAddOrder)) return false;

            ITCHAddOrder order;
            std::memcpy(&order, current_message_, sizeof(order));

            // Big-endian 변환
            order.timestamp = read_uint64_be(reinterpret_cast<const uint8_t*>(&order.timestamp));
            order.shares = read_uint32_be(reinterpret_cast<const uint8_t*>(&order.shares));
            order.price = read_uint32_be(reinterpret_cast<const uint8_t*>(&order.price));

            // Tick 구성
            std::string symbol = parse_stock_symbol(order.stock);
            tick.symbol_id = mapper ? mapper->get_symbol_id(symbol) : 0;
            tick.timestamp_ns = order.timestamp;
            tick.price = parse_price(order.price);
            tick.volume = order.shares;
            tick.side = parse_buy_sell(order.buy_sell_indicator);
            tick.type = TickType::ORDER;
            return true;
        }

        case ITCHMessageType::TRADE: {
            if (current_message_len_ < sizeof(ITCHTrade)) return false;

            ITCHTrade trade;
            std::memcpy(&trade, current_message_, sizeof(trade));

            // Big-endian 변환
            trade.timestamp = read_uint64_be(reinterpret_cast<const uint8_t*>(&trade.timestamp));
            trade.shares = read_uint32_be(reinterpret_cast<const uint8_t*>(&trade.shares));
            trade.price = read_uint32_be(reinterpret_cast<const uint8_t*>(&trade.price));

            // Tick 구성
            std::string symbol = parse_stock_symbol(trade.stock);
            tick.symbol_id = mapper ? mapper->get_symbol_id(symbol) : 0;
            tick.timestamp_ns = trade.timestamp;
            tick.price = parse_price(trade.price);
            tick.volume = trade.shares;
            tick.side = parse_buy_sell(trade.buy_sell_indicator);
            tick.type = TickType::TRADE;
            return true;
        }

        case ITCHMessageType::ORDER_EXECUTED: {
            if (current_message_len_ < sizeof(ITCHOrderExecuted)) return false;

            ITCHOrderExecuted exec;
            std::memcpy(&exec, current_message_, sizeof(exec));

            // Big-endian 변환
            exec.timestamp = read_uint64_be(reinterpret_cast<const uint8_t*>(&exec.timestamp));
            exec.executed_shares = read_uint32_be(reinterpret_cast<const uint8_t*>(&exec.executed_shares));

            tick.timestamp_ns = exec.timestamp;
            tick.volume = exec.executed_shares;
            tick.type = TickType::TRADE;
            return true;
        }

        default:
            return false;
    }
}

// ============================================================================
// 헬퍼 함수
// ============================================================================

uint16_t NASDAQITCHParser::read_uint16_be(const uint8_t* ptr) const {
    return (static_cast<uint16_t>(ptr[0]) << 8) |
           static_cast<uint16_t>(ptr[1]);
}

uint32_t NASDAQITCHParser::read_uint32_be(const uint8_t* ptr) const {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

uint64_t NASDAQITCHParser::read_uint64_be(const uint8_t* ptr) const {
    return (static_cast<uint64_t>(ptr[0]) << 56) |
           (static_cast<uint64_t>(ptr[1]) << 48) |
           (static_cast<uint64_t>(ptr[2]) << 40) |
           (static_cast<uint64_t>(ptr[3]) << 32) |
           (static_cast<uint64_t>(ptr[4]) << 24) |
           (static_cast<uint64_t>(ptr[5]) << 16) |
           (static_cast<uint64_t>(ptr[6]) << 8) |
           static_cast<uint64_t>(ptr[7]);
}

std::string NASDAQITCHParser::parse_stock_symbol(const char stock[8]) const {
    std::string symbol(stock, 8);
    // 오른쪽 공백 제거
    symbol.erase(std::find_if(symbol.rbegin(), symbol.rend(),
                              [](unsigned char ch) { return !std::isspace(ch); }).base(),
                 symbol.end());
    return symbol;
}

Side NASDAQITCHParser::parse_buy_sell(char indicator) const {
    if (indicator == 'B') return Side::BUY;
    if (indicator == 'S') return Side::SELL;
    return Side::UNKNOWN;
}

double NASDAQITCHParser::parse_price(uint32_t price) const {
    // ITCH price: 10000 단위
    // 예: 1500000 = $150.00
    return static_cast<double>(price) / 10000.0;
}

} // namespace zeptodb::feeds

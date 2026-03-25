// ============================================================================
// ZeptoDB: FIX Protocol Parser
// ============================================================================
// FIX (Financial Information eXchange) 프로토콜 파서
// 형식: 8=FIX.4.4|9=154|35=D|49=SENDER|56=TARGET|...
// ============================================================================
#pragma once

#include "zeptodb/feeds/tick.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>

namespace zeptodb::feeds {

// ============================================================================
// FIX Message Type (Tag 35)
// ============================================================================
enum class FIXMsgType {
    HEARTBEAT = 0,                // 0
    LOGON = 'A',                  // A
    LOGOUT = '5',                 // 5
    MARKET_DATA_SNAPSHOT = 'W',   // W - 호가
    MARKET_DATA_INCREMENTAL = 'X',// X - 틱 업데이트
    EXECUTION_REPORT = '8',       // 8 - 체결
    QUOTE_REQUEST = 'R',          // R
    NEW_ORDER_SINGLE = 'D',       // D
    UNKNOWN = 255
};

// ============================================================================
// FIX Tag 상수
// ============================================================================
namespace FIXTag {
    constexpr int BeginString = 8;
    constexpr int BodyLength = 9;
    constexpr int MsgType = 35;
    constexpr int SenderCompID = 49;
    constexpr int TargetCompID = 56;
    constexpr int MsgSeqNum = 34;
    constexpr int SendingTime = 52;
    constexpr int Symbol = 55;
    constexpr int Side = 54;
    constexpr int OrderQty = 38;
    constexpr int Price = 44;
    constexpr int LastPx = 31;
    constexpr int LastQty = 32;
    constexpr int TradeDate = 75;
    constexpr int TransactTime = 60;
    constexpr int BidPx = 132;
    constexpr int BidSize = 134;
    constexpr int OfferPx = 133;
    constexpr int OfferSize = 135;
    constexpr int OrderID = 37;
    constexpr int CheckSum = 10;
}

// ============================================================================
// FIX Parser
// ============================================================================
class FIXParser {
public:
    FIXParser();
    ~FIXParser() = default;

    // 메시지 파싱
    bool parse(const char* msg, size_t len);

    // 파싱된 데이터 추출
    bool extract_tick(Tick& tick, SymbolMapper* mapper) const;
    bool extract_quote(Quote& quote, SymbolMapper* mapper) const;
    bool extract_order(Order& order, SymbolMapper* mapper) const;

    // 메시지 타입
    FIXMsgType get_msg_type() const { return msg_type_; }

    // 필드 조회
    bool get_string(int tag, std::string& value) const;
    bool get_int(int tag, int64_t& value) const;
    bool get_double(int tag, double& value) const;

    // 통계
    uint64_t get_parse_count() const { return parse_count_; }
    uint64_t get_error_count() const { return error_count_; }

private:
    // 필드 맵 (tag → value)
    std::unordered_map<int, std::string> fields_;
    FIXMsgType msg_type_;

    // 통계
    uint64_t parse_count_;
    uint64_t error_count_;

    // 헬퍼
    bool validate_checksum(const char* msg, size_t len) const;
    FIXMsgType parse_msg_type(const std::string& type_str) const;
    Side parse_side(const std::string& side_str) const;
    uint64_t parse_timestamp(const std::string& time_str) const;
};

// ============================================================================
// FIX Message Builder (응답 메시지 생성용)
// ============================================================================
class FIXMessageBuilder {
public:
    FIXMessageBuilder(const std::string& sender_comp_id,
                      const std::string& target_comp_id);

    // 필드 추가
    void add_field(int tag, const std::string& value);
    void add_field(int tag, int64_t value);
    void add_field(int tag, double value);

    // 메시지 생성
    std::string build(char msg_type);

    // Logon 메시지
    std::string build_logon(int heartbeat_interval = 30);

    // Heartbeat 메시지
    std::string build_heartbeat();

    // Market Data Request (35=V)
    std::string build_market_data_request(const std::vector<std::string>& symbols);

private:
    std::string sender_comp_id_;
    std::string target_comp_id_;
    int msg_seq_num_;
    std::unordered_map<int, std::string> fields_;

    uint8_t calculate_checksum(const std::string& msg) const;
};

} // namespace zeptodb::feeds

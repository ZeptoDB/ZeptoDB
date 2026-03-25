// ============================================================================
// ZeptoDB: FIX Protocol Parser Implementation
// ============================================================================
#include "zeptodb/feeds/fix_parser.h"
#include <sstream>
#include <iomanip>
#include <ctime>

namespace zeptodb::feeds {

// ============================================================================
// FIX Parser
// ============================================================================
FIXParser::FIXParser()
    : msg_type_(FIXMsgType::UNKNOWN)
    , parse_count_(0)
    , error_count_(0)
{}

bool FIXParser::parse(const char* msg, size_t len) {
    fields_.clear();
    msg_type_ = FIXMsgType::UNKNOWN;

    if (!msg || len == 0) {
        ++error_count_;
        return false;
    }

    // FIX 메시지는 SOH (0x01)로 필드 구분
    constexpr char SOH = 0x01;
    const char* ptr = msg;
    const char* end = msg + len;

    while (ptr < end) {
        // tag 파싱
        int tag = 0;
        while (ptr < end && *ptr >= '0' && *ptr <= '9') {
            tag = tag * 10 + (*ptr - '0');
            ++ptr;
        }

        if (ptr >= end || *ptr != '=') {
            ++error_count_;
            return false;
        }
        ++ptr; // skip '='

        // value 파싱 (SOH까지)
        const char* value_start = ptr;
        while (ptr < end && *ptr != SOH) {
            ++ptr;
        }

        std::string value(value_start, ptr - value_start);
        fields_[tag] = value;

        // MsgType 저장
        if (tag == FIXTag::MsgType && !value.empty()) {
            msg_type_ = parse_msg_type(value);
        }

        if (ptr < end) ++ptr; // skip SOH
    }

    // Checksum 검증 (옵션)
    // if (!validate_checksum(msg, len)) {
    //     ++error_count_;
    //     return false;
    // }

    ++parse_count_;
    return true;
}

bool FIXParser::extract_tick(Tick& tick, SymbolMapper* mapper) const {
    if (msg_type_ != FIXMsgType::EXECUTION_REPORT &&
        msg_type_ != FIXMsgType::MARKET_DATA_INCREMENTAL) {
        return false;
    }

    // Symbol
    std::string symbol;
    if (!get_string(FIXTag::Symbol, symbol)) return false;
    tick.symbol_id = mapper ? mapper->get_symbol_id(symbol) : 0;

    // Price (LastPx or Price)
    if (!get_double(FIXTag::LastPx, tick.price)) {
        if (!get_double(FIXTag::Price, tick.price)) {
            return false;
        }
    }

    // Volume (LastQty or OrderQty)
    int64_t vol;
    if (get_int(FIXTag::LastQty, vol)) {
        tick.volume = vol;
    } else if (get_int(FIXTag::OrderQty, vol)) {
        tick.volume = vol;
    } else {
        return false;
    }

    // Side
    std::string side_str;
    if (get_string(FIXTag::Side, side_str)) {
        tick.side = parse_side(side_str);
    }

    // Timestamp
    std::string time_str;
    if (get_string(FIXTag::TransactTime, time_str)) {
        tick.timestamp_ns = parse_timestamp(time_str);
    } else {
        tick.timestamp_ns = now_ns();
    }

    tick.type = TickType::TRADE;
    return true;
}

bool FIXParser::extract_quote(Quote& quote, SymbolMapper* mapper) const {
    if (msg_type_ != FIXMsgType::MARKET_DATA_SNAPSHOT) {
        return false;
    }

    // Symbol
    std::string symbol;
    if (!get_string(FIXTag::Symbol, symbol)) return false;
    quote.symbol_id = mapper ? mapper->get_symbol_id(symbol) : 0;

    // Bid
    if (!get_double(FIXTag::BidPx, quote.bid_price)) return false;
    int64_t bid_vol;
    if (get_int(FIXTag::BidSize, bid_vol)) {
        quote.bid_volume = bid_vol;
    }

    // Ask
    if (!get_double(FIXTag::OfferPx, quote.ask_price)) return false;
    int64_t ask_vol;
    if (get_int(FIXTag::OfferSize, ask_vol)) {
        quote.ask_volume = ask_vol;
    }

    // Timestamp
    std::string time_str;
    if (get_string(FIXTag::SendingTime, time_str)) {
        quote.timestamp_ns = parse_timestamp(time_str);
    } else {
        quote.timestamp_ns = now_ns();
    }

    return true;
}

bool FIXParser::extract_order(Order& order, SymbolMapper* mapper) const {
    if (msg_type_ != FIXMsgType::NEW_ORDER_SINGLE) {
        return false;
    }

    // Symbol
    std::string symbol;
    if (!get_string(FIXTag::Symbol, symbol)) return false;
    order.symbol_id = mapper ? mapper->get_symbol_id(symbol) : 0;

    // Price
    if (!get_double(FIXTag::Price, order.price)) return false;

    // Volume
    int64_t vol;
    if (!get_int(FIXTag::OrderQty, vol)) return false;
    order.volume = vol;

    // Side
    std::string side_str;
    if (get_string(FIXTag::Side, side_str)) {
        order.side = parse_side(side_str);
    }

    // Order ID
    int64_t order_id;
    if (get_int(FIXTag::OrderID, order_id)) {
        order.order_id = order_id;
    }

    order.timestamp_ns = now_ns();
    return true;
}

bool FIXParser::get_string(int tag, std::string& value) const {
    auto it = fields_.find(tag);
    if (it == fields_.end()) return false;
    value = it->second;
    return true;
}

bool FIXParser::get_int(int tag, int64_t& value) const {
    std::string str;
    if (!get_string(tag, str)) return false;
    try {
        value = std::stoll(str);
        return true;
    } catch (...) {
        return false;
    }
}

bool FIXParser::get_double(int tag, double& value) const {
    std::string str;
    if (!get_string(tag, str)) return false;
    try {
        value = std::stod(str);
        return true;
    } catch (...) {
        return false;
    }
}

bool FIXParser::validate_checksum(const char* msg, size_t len) const {
    // Checksum은 10=XXX 형태 (마지막 필드)
    // 전체 메시지의 바이트 합 % 256
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        if (msg[i] == 0x01 && i + 4 < len &&
            msg[i+1] == '1' && msg[i+2] == '0' && msg[i+3] == '=') {
            break;
        }
        sum += static_cast<uint8_t>(msg[i]);
    }

    std::string checksum_str;
    if (!get_string(FIXTag::CheckSum, checksum_str)) return false;

    int expected = std::stoi(checksum_str);
    return (sum % 256) == expected;
}

FIXMsgType FIXParser::parse_msg_type(const std::string& type_str) const {
    if (type_str.empty()) return FIXMsgType::UNKNOWN;
    char c = type_str[0];

    switch (c) {
        case '0': return FIXMsgType::HEARTBEAT;
        case 'A': return FIXMsgType::LOGON;
        case '5': return FIXMsgType::LOGOUT;
        case 'W': return FIXMsgType::MARKET_DATA_SNAPSHOT;
        case 'X': return FIXMsgType::MARKET_DATA_INCREMENTAL;
        case '8': return FIXMsgType::EXECUTION_REPORT;
        case 'R': return FIXMsgType::QUOTE_REQUEST;
        case 'D': return FIXMsgType::NEW_ORDER_SINGLE;
        default:  return FIXMsgType::UNKNOWN;
    }
}

Side FIXParser::parse_side(const std::string& side_str) const {
    if (side_str.empty()) return Side::UNKNOWN;
    char c = side_str[0];

    if (c == '1' || c == 'B') return Side::BUY;
    if (c == '2' || c == 'S') return Side::SELL;
    return Side::UNKNOWN;
}

uint64_t FIXParser::parse_timestamp(const std::string& time_str) const {
    // FIX 타임스탬프 형식: YYYYMMDD-HH:MM:SS.mmm
    // 예: 20260322-08:30:45.123
    if (time_str.length() < 17) return now_ns();

    std::tm tm = {};
    tm.tm_year = std::stoi(time_str.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(time_str.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(time_str.substr(6, 2));
    tm.tm_hour = std::stoi(time_str.substr(9, 2));
    tm.tm_min = std::stoi(time_str.substr(12, 2));
    tm.tm_sec = std::stoi(time_str.substr(15, 2));

    auto epoch = std::mktime(&tm);
    uint64_t ns = static_cast<uint64_t>(epoch) * 1000000000ULL;

    // 밀리초 추가
    if (time_str.length() >= 21) {
        int millis = std::stoi(time_str.substr(18, 3));
        ns += millis * 1000000ULL;
    }

    return ns;
}

// ============================================================================
// FIX Message Builder
// ============================================================================
FIXMessageBuilder::FIXMessageBuilder(const std::string& sender_comp_id,
                                     const std::string& target_comp_id)
    : sender_comp_id_(sender_comp_id)
    , target_comp_id_(target_comp_id)
    , msg_seq_num_(1)
{}

void FIXMessageBuilder::add_field(int tag, const std::string& value) {
    fields_[tag] = value;
}

void FIXMessageBuilder::add_field(int tag, int64_t value) {
    fields_[tag] = std::to_string(value);
}

void FIXMessageBuilder::add_field(int tag, double value) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(5) << value;
    fields_[tag] = os.str();
}

std::string FIXMessageBuilder::build(char msg_type) {
    constexpr char SOH = 0x01;
    std::ostringstream body;

    // 필수 헤더
    body << FIXTag::MsgType << "=" << msg_type << SOH;
    body << FIXTag::SenderCompID << "=" << sender_comp_id_ << SOH;
    body << FIXTag::TargetCompID << "=" << target_comp_id_ << SOH;
    body << FIXTag::MsgSeqNum << "=" << msg_seq_num_++ << SOH;

    // SendingTime (YYYYMMDD-HH:MM:SS)
    auto now = std::time(nullptr);
    std::tm tm;
    gmtime_r(&now, &tm);
    std::ostringstream time_os;
    time_os << std::put_time(&tm, "%Y%m%d-%H:%M:%S");
    body << FIXTag::SendingTime << "=" << time_os.str() << SOH;

    // 사용자 필드
    for (const auto& [tag, value] : fields_) {
        body << tag << "=" << value << SOH;
    }

    std::string body_str = body.str();

    // 전체 메시지
    std::ostringstream msg;
    msg << FIXTag::BeginString << "=FIX.4.4" << SOH;
    msg << FIXTag::BodyLength << "=" << body_str.length() << SOH;
    msg << body_str;

    // Checksum
    uint8_t checksum = calculate_checksum(msg.str());
    msg << FIXTag::CheckSum << "=" << std::setfill('0') << std::setw(3) << (int)checksum << SOH;

    fields_.clear();
    return msg.str();
}

std::string FIXMessageBuilder::build_logon(int heartbeat_interval) {
    add_field(98, int64_t{0});  // EncryptMethod (0=None)
    add_field(108, int64_t{heartbeat_interval});  // HeartBtInt
    return build('A');
}

std::string FIXMessageBuilder::build_heartbeat() {
    return build('0');
}

std::string FIXMessageBuilder::build_market_data_request(const std::vector<std::string>& symbols) {
    // 263=1 (SubscriptionRequestType: Snapshot + Updates)
    add_field(263, int64_t{1});
    // 264=0 (MarketDepth: Full book)
    add_field(264, int64_t{0});
    // 267=2 (NoMDEntryTypes: Bid + Offer)
    add_field(267, int64_t{2});
    add_field(269, "0");  // MDEntryType: Bid
    add_field(269, "1");  // MDEntryType: Offer
    // 146=N (NoRelatedSym)
    add_field(146, static_cast<int64_t>(symbols.size()));
    for (const auto& sym : symbols)
        add_field(55, sym);  // Symbol
    return build('V');
}

uint8_t FIXMessageBuilder::calculate_checksum(const std::string& msg) const {
    uint32_t sum = 0;
    for (char c : msg) {
        sum += static_cast<uint8_t>(c);
    }
    return sum % 256;
}

} // namespace zeptodb::feeds

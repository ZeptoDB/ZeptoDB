// ============================================================================
// APEX-DB: Feed Handler 인터페이스
// ============================================================================
#pragma once

#include "apex/feeds/tick.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apex::feeds {

// ============================================================================
// Feed Handler 콜백
// ============================================================================
using TickCallback = std::function<void(const Tick&)>;
using QuoteCallback = std::function<void(const Quote&)>;
using OrderCallback = std::function<void(const Order&)>;
using ErrorCallback = std::function<void(const std::string&)>;

// ============================================================================
// Feed Handler Status
// ============================================================================
enum class FeedStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    SUBSCRIBING,
    ACTIVE,
    ERROR
};

// ============================================================================
// Feed Handler 인터페이스
// ============================================================================
class IFeedHandler {
public:
    virtual ~IFeedHandler() = default;

    // 연결 관리
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // 구독 관리
    virtual bool subscribe(const std::vector<std::string>& symbols) = 0;
    virtual bool unsubscribe(const std::vector<std::string>& symbols) = 0;

    // 콜백 등록
    virtual void on_tick(TickCallback callback) = 0;
    virtual void on_quote(QuoteCallback callback) = 0;
    virtual void on_order(OrderCallback callback) = 0;
    virtual void on_error(ErrorCallback callback) = 0;

    // 상태
    virtual FeedStatus get_status() const = 0;

    // 통계
    virtual uint64_t get_message_count() const = 0;
    virtual uint64_t get_byte_count() const = 0;
    virtual uint64_t get_error_count() const = 0;
};

// ============================================================================
// Feed Handler 설정
// ============================================================================
struct FeedConfig {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
    bool use_tls = false;
    int reconnect_interval_ms = 5000;
    int heartbeat_interval_ms = 30000;
    size_t buffer_size = 65536;  // 64KB
};

// ============================================================================
// Feed Handler 팩토리
// ============================================================================
enum class FeedType {
    FIX,
    MULTICAST_UDP,
    NASDAQ_ITCH,
    CME_SBE,
    BINANCE_WS
};

std::unique_ptr<IFeedHandler> create_feed_handler(
    FeedType type,
    const FeedConfig& config,
    SymbolMapper* mapper
);

} // namespace apex::feeds

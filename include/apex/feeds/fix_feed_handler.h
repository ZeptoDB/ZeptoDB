// ============================================================================
// APEX-DB: FIX Feed Handler
// ============================================================================
#pragma once

#include "apex/feeds/feed_handler.h"
#include "apex/feeds/fix_parser.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace apex::feeds {

// ============================================================================
// FIX Feed Handler (TCP 기반)
// ============================================================================
class FIXFeedHandler : public IFeedHandler {
public:
    FIXFeedHandler(const FeedConfig& config, SymbolMapper* mapper);
    ~FIXFeedHandler() override;

    // IFeedHandler 인터페이스
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    bool subscribe(const std::vector<std::string>& symbols) override;
    bool unsubscribe(const std::vector<std::string>& symbols) override;

    void on_tick(TickCallback callback) override;
    void on_quote(QuoteCallback callback) override;
    void on_order(OrderCallback callback) override;
    void on_error(ErrorCallback callback) override;

    FeedStatus get_status() const override;

    uint64_t get_message_count() const override { return message_count_.load(); }
    uint64_t get_byte_count() const override { return byte_count_.load(); }
    uint64_t get_error_count() const override { return error_count_.load(); }

private:
    FeedConfig config_;
    SymbolMapper* mapper_;
    FIXParser parser_;
    FIXMessageBuilder builder_;

    // 소켓
    int sockfd_;
    std::atomic<FeedStatus> status_;
    std::atomic<bool> running_;

    // 콜백
    std::mutex callback_mutex_;
    TickCallback tick_callback_;
    QuoteCallback quote_callback_;
    OrderCallback order_callback_;
    ErrorCallback error_callback_;

    // 구독 리스트
    std::mutex symbols_mutex_;
    std::vector<std::string> subscribed_symbols_;

    // 워커 스레드
    std::thread recv_thread_;
    std::thread heartbeat_thread_;

    // 통계
    std::atomic<uint64_t> message_count_{0};
    std::atomic<uint64_t> byte_count_{0};
    std::atomic<uint64_t> error_count_{0};

    // 내부 메서드
    bool connect_socket();
    void close_socket();
    bool send_logon();
    bool send_heartbeat();
    bool send_market_data_request(const std::vector<std::string>& symbols);

    void recv_loop();
    void heartbeat_loop();
    void handle_message(const char* msg, size_t len);
    void notify_error(const std::string& error);
};

} // namespace apex::feeds

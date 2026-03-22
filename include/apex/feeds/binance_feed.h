// ============================================================================
// APEX-DB: Binance WebSocket Feed Handler
// ============================================================================
// Binance Spot Market WebSocket Streams
// wss://stream.binance.com:9443/ws/<streamName>
// ============================================================================
#pragma once

#include "apex/feeds/feed_handler.h"
#include <string>

namespace apex::feeds {

// ============================================================================
// Binance WebSocket Feed Handler
// ============================================================================
// TODO: WebSocket 라이브러리 필요 (e.g., websocketpp, boost::beast)
// 현재는 인터페이스만 정의
class BinanceFeedHandler : public IFeedHandler {
public:
    BinanceFeedHandler(const FeedConfig& config, SymbolMapper* mapper);
    ~BinanceFeedHandler() override = default;

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

    uint64_t get_message_count() const override;
    uint64_t get_byte_count() const override;
    uint64_t get_error_count() const override;

private:
    FeedConfig config_;
    SymbolMapper* mapper_;
    FeedStatus status_;

    TickCallback tick_callback_;
    QuoteCallback quote_callback_;
    OrderCallback order_callback_;
    ErrorCallback error_callback_;

    uint64_t message_count_;
    uint64_t byte_count_;
    uint64_t error_count_;

    // WebSocket 스트림 URL 생성
    // 예: btcusdt@trade, btcusdt@depth
    std::string build_stream_url(const std::vector<std::string>& symbols);

    // JSON 파싱 (trade, depth 메시지)
    void parse_trade_message(const std::string& json);
    void parse_depth_message(const std::string& json);
};

// ============================================================================
// Binance Stream Types
// ============================================================================
enum class BinanceStreamType {
    TRADE,          // @trade - 실시간 체결
    AGG_TRADE,      // @aggTrade - 집계 체결
    KLINE,          // @kline_1m - K라인/캔들
    MINI_TICKER,    // @miniTicker - 24시간 통계
    TICKER,         // @ticker - 24시간 통계 (전체)
    BOOK_TICKER,    // @bookTicker - 최우선 호가
    DEPTH,          // @depth - 호가창
    DEPTH_UPDATE    // @depth@100ms - 호가 업데이트
};

} // namespace apex::feeds

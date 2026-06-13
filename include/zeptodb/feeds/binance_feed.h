// ============================================================================
// ZeptoDB: Binance WebSocket Feed Handler
// ============================================================================
// Binance Spot Market WebSocket Streams
// wss://stream.binance.com:9443/ws/<streamName>
// ============================================================================
#pragma once

#include "zeptodb/feeds/feed_handler.h"
#include <string>

namespace zeptodb::feeds {

// ============================================================================
// Binance WebSocket Feed Handler
// ============================================================================
// TODO: Add a WebSocket library dependency, such as websocketpp or boost::beast.
// This header currently defines the interface only.
class BinanceFeedHandler : public IFeedHandler {
public:
    BinanceFeedHandler(const FeedConfig& config, SymbolMapper* mapper);
    ~BinanceFeedHandler() override = default;

    // IFeedHandler interface.
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

    // Table-aware ingest (Stage B, devlog 084).
    // Setter is a write-only hint for callers that forward Ticks into a
    // ZeptoDB pipeline; they should stamp `table_id()` onto the produced
    // TickMessage before ingest.
    void     set_table_id(uint16_t tid)           { table_id_ = tid; }
    void     set_table_name(std::string name)     { table_name_ = std::move(name); }
    uint16_t table_id()    const { return table_id_; }
    const std::string& table_name() const { return table_name_; }

    // devlog 088: JSON parse entry points promoted to public so unit tests
    // can verify the `table_id` stamping invariant without a live WebSocket.
    // Once the full WS transport lands, these should be callable only by
    // the internal read loop; leaving them public is a minor API surface
    // cost; the alternative (`friend` of a test-only class) is uglier.
    // TODO(devlog 088): demote to private once WebSocket transport lands;
    // promoted to public solely for unit-test reachability.
    void parse_trade_message(const std::string& json);
    void parse_depth_message(const std::string& json);

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

    // Table-aware ingest (Stage B)
    uint16_t    table_id_ = 0;
    std::string table_name_;

    // Build the WebSocket stream URL.
    // Examples: btcusdt@trade, btcusdt@depth.
    std::string build_stream_url(const std::vector<std::string>& symbols);
};

// ============================================================================
// Binance Stream Types
// ============================================================================
enum class BinanceStreamType {
    TRADE,          // @trade - real-time trade.
    AGG_TRADE,      // @aggTrade - aggregate trade.
    KLINE,          // @kline_1m - kline/candlestick.
    MINI_TICKER,    // @miniTicker - 24-hour mini ticker.
    TICKER,         // @ticker - full 24-hour ticker.
    BOOK_TICKER,    // @bookTicker - best bid/ask.
    DEPTH,          // @depth - order book depth.
    DEPTH_UPDATE    // @depth@100ms - order book update.
};

} // namespace zeptodb::feeds

// ============================================================================
// ZeptoDB: Binance WebSocket Feed Handler Implementation
// ============================================================================
// devlog 088: minimal .cpp bridge so that `table_id_` is provably stamped
// onto every emitted `Tick` before `tick_callback_` fires. The full
// WebSocket transport is still stubbed (a library dep decision) — this
// file covers just the parse → tick → callback contract.
// ============================================================================
#include "zeptodb/feeds/binance_feed.h"

#include <cctype>
#include <cstring>
#include <string>

namespace zeptodb::feeds {

BinanceFeedHandler::BinanceFeedHandler(const FeedConfig& config,
                                       SymbolMapper* mapper)
    : config_(config)
    , mapper_(mapper)
    , status_(FeedStatus::DISCONNECTED)
    , message_count_(0)
    , byte_count_(0)
    , error_count_(0)
{}

// Transport is not implemented here; these keep the vtable non-abstract so
// the class is instantiable (and set_table_id is exercisable in tests).
bool BinanceFeedHandler::connect()                              { return false; }
void BinanceFeedHandler::disconnect()                           {}
bool BinanceFeedHandler::is_connected() const                   { return false; }
bool BinanceFeedHandler::subscribe(const std::vector<std::string>&)   { return false; }
bool BinanceFeedHandler::unsubscribe(const std::vector<std::string>&) { return false; }

void BinanceFeedHandler::on_tick(TickCallback cb)   { tick_callback_   = std::move(cb); }
void BinanceFeedHandler::on_quote(QuoteCallback cb) { quote_callback_  = std::move(cb); }
void BinanceFeedHandler::on_order(OrderCallback cb) { order_callback_  = std::move(cb); }
void BinanceFeedHandler::on_error(ErrorCallback cb) { error_callback_  = std::move(cb); }

FeedStatus BinanceFeedHandler::get_status() const   { return status_; }
uint64_t BinanceFeedHandler::get_message_count() const { return message_count_; }
uint64_t BinanceFeedHandler::get_byte_count() const    { return byte_count_; }
uint64_t BinanceFeedHandler::get_error_count() const   { return error_count_; }

std::string BinanceFeedHandler::build_stream_url(
    const std::vector<std::string>& symbols)
{
    // Minimal placeholder — ws:// base + lowercased "<sym>@trade" streams.
    std::string url = "wss://stream.binance.com:9443/stream?streams=";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i) url += '/';
        std::string s = symbols[i];
        for (auto& c : s) c = static_cast<char>(std::tolower(
            static_cast<unsigned char>(c)));
        url += s + "@trade";
    }
    return url;
}

// ----------------------------------------------------------------------------
// Trade JSON parse (minimal: extracts `s`/`p`/`q`/`T`/`m`) and emits a Tick
// with `table_id = table_id_` stamped *before* `tick_callback_`.
//
// Expected payload shape (Binance spot `@trade`):
//   {"e":"trade","E":ts,"s":"BTCUSDT","t":12345,"p":"30000.10",
//    "q":"0.001","b":..,"a":..,"T":ts_ms,"m":true}
//
// This is a deliberately minimal hand-rolled extractor — no JSON dep — so
// the stamping path is linkable in unit tests without dragging in a
// WebSocket library.
// ----------------------------------------------------------------------------
namespace {

bool extract_str(const std::string& j, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\":\"";
    auto p = j.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    auto q = j.find('"', p);
    if (q == std::string::npos) return false;
    out = j.substr(p, q - p);
    return true;
}

bool extract_num_str(const std::string& j, const char* key, std::string& out) {
    // numeric field (no quotes): "key":123
    const std::string needle = std::string("\"") + key + "\":";
    auto p = j.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    auto q = p;
    while (q < j.size() && (std::isdigit(static_cast<unsigned char>(j[q])) ||
                            j[q] == '-' || j[q] == '.')) ++q;
    if (q == p) return false;
    out = j.substr(p, q - p);
    return true;
}

}  // namespace

void BinanceFeedHandler::parse_trade_message(const std::string& json) {
    ++message_count_;
    byte_count_ += json.size();

    Tick tick;
    // devlog 088: stamp table_id onto the emitted tick so downstream pipeline
    // ingest routes to the correct SchemaRegistry-assigned table. 0 = legacy.
    tick.table_id = table_id_;
    tick.type     = TickType::TRADE;

    std::string sym, price_s, qty_s, ts_ms_s, is_buyer_maker_s;
    if (extract_str(json, "s", sym)) {
        tick.symbol_id = mapper_ ? mapper_->get_symbol_id(sym) : 0;
    }
    if (extract_str(json, "p", price_s)) {
        try { tick.price = std::stod(price_s); } catch (...) { ++error_count_; }
    }
    if (extract_str(json, "q", qty_s)) {
        // NOTE: truncates fractional qty (e.g., "0.001" → 0). Acceptable for
        // integer-volume schemas; for sub-unit precision, multiply by a scale
        // factor before cast (see Binance qty precision backlog item).
        try { tick.volume = static_cast<uint64_t>(std::stod(qty_s)); }
        catch (...) { ++error_count_; }
    }
    if (extract_num_str(json, "T", ts_ms_s)) {
        try { tick.timestamp_ns = millis_to_nanos(std::stoull(ts_ms_s)); }
        catch (...) { tick.timestamp_ns = now_ns(); }
    } else {
        tick.timestamp_ns = now_ns();
    }
    // "m" is boolean true/false — true => buyer is market maker => sell side
    auto mpos = json.find("\"m\":");
    if (mpos != std::string::npos) {
        tick.side = (json.compare(mpos + 4, 4, "true") == 0)
                      ? Side::SELL : Side::BUY;
    }

    if (tick_callback_) tick_callback_(tick);
}

void BinanceFeedHandler::parse_depth_message(const std::string& /*json*/) {
    // Depth -> Quote mapping isn't part of the devlog 088 contract; the
    // invariant this file exists to prove is table_id stamping on Tick.
    ++message_count_;
}

} // namespace zeptodb::feeds

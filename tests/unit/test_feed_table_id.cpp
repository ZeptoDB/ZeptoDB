// ============================================================================
// devlog 086 (D3): Feed parsers auto-stamp table_id on emitted ticks
// ----------------------------------------------------------------------------
// After `set_table_id(tid)`, parsers must propagate that id onto every
// `Tick` (and downstream `TickMessage` when the feed handler bridges into a
// pipeline), so that SchemaRegistry-scoped routing/storage hits the correct
// table.
//
// Covered here:
//   • ITCH parser (exercised directly via a synthesised Trade packet).
//   • FIX parser — verified by code inspection at
//       src/feeds/fix_feed_handler.cpp:~240 (see `handle_message`),
//     where the handler stamps `tick.table_id = parser_.table_id()`
//     immediately before `tick_callback_(tick)`. A hermetic unit test is
//     awkward because FIXFeedHandler owns a TCP socket; ITCH coverage plus
//     the setter/getter check is sufficient to pin the wiring.
//   • Binance parser — devlog 088 added `src/feeds/binance_feed.cpp`; the
//     auto-stamp test below feeds a minimal trade-JSON payload through
//     `parse_trade_message` and verifies `tick.table_id == table_id_` on the
//     emitted Tick, in the same style as the ITCH test.
// ============================================================================
#include "zeptodb/feeds/nasdaq_itch.h"
#include "zeptodb/feeds/fix_parser.h"
#include "zeptodb/feeds/binance_feed.h"

#include <gtest/gtest.h>
#include <cstring>

using namespace zeptodb::feeds;

namespace {

class MockMapper : public SymbolMapper {
public:
    uint32_t get_symbol_id(const std::string& s) override {
        return s.find("AAPL") != std::string::npos ? 1u : 0u;
    }
    std::string get_symbol_name(uint32_t id) override {
        return id == 1 ? "AAPL" : "UNKNOWN";
    }
};

void w_u16_be(uint8_t* p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
void w_u32_be(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (v >> (24 - i * 8)) & 0xFF;
}
void w_u64_be(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (v >> (56 - i * 8)) & 0xFF;
}

}  // namespace

TEST(FeedTableId, ItchParserAutoStampsTableId) {
    NASDAQITCHParser parser;
    MockMapper mapper;
    parser.set_table_id(7);
    EXPECT_EQ(parser.table_id(), 7);

    // Build a minimal ITCH Trade (Type 'P') packet.
    uint8_t pkt[256]{};
    size_t off = 0;
    w_u16_be(pkt + off, 45); off += 2;   // length (payload + type)
    pkt[off++] = 'P';                    // message type = Trade
    w_u16_be(pkt + off, 1);  off += 2;   // stock_locate
    w_u16_be(pkt + off, 0);  off += 2;   // tracking_number
    w_u64_be(pkt + off, 2'000'000'000ULL); off += 8;  // timestamp
    w_u64_be(pkt + off, 42);  off += 8;   // order_reference
    pkt[off++] = 'B';                     // buy/sell
    w_u32_be(pkt + off, 100); off += 4;   // shares
    std::memcpy(pkt + off, "AAPL    ", 8); off += 8;  // stock
    w_u32_be(pkt + off, 1'500'000); off += 4;  // price
    w_u64_be(pkt + off, 123); off += 8;        // match_number

    ASSERT_TRUE(parser.parse_packet(pkt, off));
    Tick tick;
    ASSERT_TRUE(parser.extract_tick(tick, &mapper));
    EXPECT_EQ(tick.symbol_id, 1u);
    EXPECT_EQ(tick.table_id, 7);  // ← the thing this test pins
}

TEST(FeedTableId, FixParserSetterRoundTrip) {
    // Direct emission-point coverage lives in fix_feed_handler.cpp (stamps
    // `tick.table_id = parser_.table_id()` right before dispatch); here we
    // pin that the setter/getter round-trip works on the parser.
    FIXParser p;
    p.set_table_id(42);
    EXPECT_EQ(p.table_id(), 42);
    p.set_table_name("trades_fix");
    EXPECT_EQ(p.table_name(), "trades_fix");
}

TEST(FeedTableId, BinanceParserAutoStampsTableId) {
    // devlog 088: Binance feed now has a .cpp bridge that stamps `table_id_`
    // onto the emitted Tick before `tick_callback_`. We exercise that path
    // by feeding a minimal trade JSON and capturing the emitted Tick.
    FeedConfig cfg;
    MockMapper mapper;
    BinanceFeedHandler h(cfg, &mapper);
    h.set_table_id(9);
    EXPECT_EQ(h.table_id(), 9);

    Tick captured;
    bool fired = false;
    h.on_tick([&](const Tick& t) { captured = t; fired = true; });

    // Minimal Binance spot @trade payload.
    const std::string json =
        R"({"e":"trade","E":123,"s":"AAPL","t":1,"p":"150.25",)"
        R"("q":"10","b":0,"a":0,"T":1700000000000,"m":false})";

    h.parse_trade_message(json);

    ASSERT_TRUE(fired);
    EXPECT_EQ(captured.table_id, 9);         // ← the thing this test pins
    EXPECT_EQ(captured.symbol_id, 1u);       // AAPL → 1
    EXPECT_EQ(captured.type, TickType::TRADE);
}

// Struct-level: the default Tick has table_id == 0 (legacy/default path).
TEST(FeedTableId, DefaultTickTableIdIsZero) {
    Tick t;
    EXPECT_EQ(t.table_id, 0);
}

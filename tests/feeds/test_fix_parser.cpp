// ============================================================================
// ZeptoDB: FIX Parser Unit Tests
// ============================================================================
#include "zeptodb/feeds/fix_parser.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace zeptodb::feeds;

// ============================================================================
// Mock Symbol Mapper
// ============================================================================
class MockSymbolMapper : public SymbolMapper {
public:
    uint32_t get_symbol_id(const std::string& symbol) override {
        if (symbol == "AAPL") return 1;
        if (symbol == "MSFT") return 2;
        if (symbol == "TSLA") return 3;
        return 0;
    }

    std::string get_symbol_name(uint32_t symbol_id) override {
        if (symbol_id == 1) return "AAPL";
        if (symbol_id == 2) return "MSFT";
        if (symbol_id == 3) return "TSLA";
        return "UNKNOWN";
    }
};

// ============================================================================
// FIX Parser Basic Tests
// ============================================================================
class FIXParserTest : public ::testing::Test {
protected:
    FIXParser parser_;
    MockSymbolMapper mapper_;
};

TEST_F(FIXParserTest, ParseBasicMessage) {
    // FIX message: Execution Report (35=8)
    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=8\x01"
                      "49=SENDER\x01" "56=TARGET\x01" "34=1\x01"
                      "52=20260322-08:30:00\x01" "55=AAPL\x01"
                      "31=150.50\x01" "32=100\x01" "10=123\x01";

    ASSERT_TRUE(parser_.parse(msg, std::strlen(msg)));
    EXPECT_EQ(parser_.get_msg_type(), FIXMsgType::EXECUTION_REPORT);

    // 필드 조회
    std::string symbol;
    ASSERT_TRUE(parser_.get_string(FIXTag::Symbol, symbol));
    EXPECT_EQ(symbol, "AAPL");

    double price;
    ASSERT_TRUE(parser_.get_double(FIXTag::LastPx, price));
    EXPECT_DOUBLE_EQ(price, 150.50);

    int64_t qty;
    ASSERT_TRUE(parser_.get_int(FIXTag::LastQty, qty));
    EXPECT_EQ(qty, 100);
}

TEST_F(FIXParserTest, ExtractTick) {
    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=8\x01"
                      "55=AAPL\x01" "31=150.50\x01" "32=100\x01"
                      "54=1\x01" "60=20260322-08:30:00\x01" "10=123\x01";

    ASSERT_TRUE(parser_.parse(msg, std::strlen(msg)));

    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_EQ(tick.symbol_id, 1);  // AAPL
    EXPECT_DOUBLE_EQ(tick.price, 150.50);
    EXPECT_EQ(tick.volume, 100);
    EXPECT_EQ(tick.side, Side::BUY);  // 54=1
    EXPECT_EQ(tick.type, TickType::TRADE);
}

TEST_F(FIXParserTest, ExtractQuote) {
    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=W\x01"
                      "55=MSFT\x01"
                      "132=150.00\x01" "134=500\x01"    // Bid
                      "133=150.50\x01" "135=300\x01"    // Ask
                      "10=123\x01";

    ASSERT_TRUE(parser_.parse(msg, std::strlen(msg)));

    Quote quote;
    ASSERT_TRUE(parser_.extract_quote(quote, &mapper_));

    EXPECT_EQ(quote.symbol_id, 2);  // MSFT
    EXPECT_DOUBLE_EQ(quote.bid_price, 150.00);
    EXPECT_EQ(quote.bid_volume, 500);
    EXPECT_DOUBLE_EQ(quote.ask_price, 150.50);
    EXPECT_EQ(quote.ask_volume, 300);
}

TEST_F(FIXParserTest, ParseSide) {
    // Buy
    const char* msg_buy = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                          "31=150\x01" "32=100\x01" "54=1\x01" "10=123\x01";
    ASSERT_TRUE(parser_.parse(msg_buy, std::strlen(msg_buy)));
    Tick tick_buy;
    ASSERT_TRUE(parser_.extract_tick(tick_buy, &mapper_));
    EXPECT_EQ(tick_buy.side, Side::BUY);

    // Sell
    const char* msg_sell = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                           "31=150\x01" "32=100\x01" "54=2\x01" "10=123\x01";
    ASSERT_TRUE(parser_.parse(msg_sell, std::strlen(msg_sell)));
    Tick tick_sell;
    ASSERT_TRUE(parser_.extract_tick(tick_sell, &mapper_));
    EXPECT_EQ(tick_sell.side, Side::SELL);
}

TEST_F(FIXParserTest, ParseTimestamp) {
    const char* msg = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                      "31=150\x01" "32=100\x01"
                      "60=20260322-08:30:45.123\x01" "10=123\x01";

    ASSERT_TRUE(parser_.parse(msg, std::strlen(msg)));
    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    // 타임스탬프가 0이 아님 (실제 파싱됨)
    EXPECT_GT(tick.timestamp_ns, 0);
}

TEST_F(FIXParserTest, InvalidMessage) {
    // 잘못된 형식 (=가 없음)
    const char* msg = "8FIX.4.4\x01" "35=8\x01";
    EXPECT_FALSE(parser_.parse(msg, std::strlen(msg)));
    EXPECT_GT(parser_.get_error_count(), 0);
}

TEST_F(FIXParserTest, EmptyMessage) {
    EXPECT_FALSE(parser_.parse("", 0));
}

TEST_F(FIXParserTest, MultipleMessages) {
    // 첫 번째 메시지
    const char* msg1 = "8=FIX.4.4\x01" "35=8\x01" "55=AAPL\x01"
                       "31=150\x01" "32=100\x01" "10=123\x01";
    ASSERT_TRUE(parser_.parse(msg1, std::strlen(msg1)));
    EXPECT_EQ(parser_.get_parse_count(), 1);

    // 두 번째 메시지
    const char* msg2 = "8=FIX.4.4\x01" "35=8\x01" "55=MSFT\x01"
                       "31=200\x01" "32=200\x01" "10=123\x01";
    ASSERT_TRUE(parser_.parse(msg2, std::strlen(msg2)));
    EXPECT_EQ(parser_.get_parse_count(), 2);
}

// ============================================================================
// FIX Message Builder Tests
// ============================================================================
TEST(FIXMessageBuilderTest, BuildLogon) {
    FIXMessageBuilder builder("ZEPTO", "SERVER");
    std::string logon = builder.build_logon(30);

    // 기본 구조 검증
    EXPECT_TRUE(logon.find("8=FIX.4.4") != std::string::npos);
    EXPECT_TRUE(logon.find("35=A") != std::string::npos);  // Logon
    EXPECT_TRUE(logon.find("49=ZEPTO") != std::string::npos);
    EXPECT_TRUE(logon.find("56=SERVER") != std::string::npos);
    EXPECT_TRUE(logon.find("108=30") != std::string::npos);  // HeartBtInt
}

TEST(FIXMessageBuilderTest, BuildHeartbeat) {
    FIXMessageBuilder builder("ZEPTO", "SERVER");
    std::string hb = builder.build_heartbeat();

    EXPECT_TRUE(hb.find("35=0") != std::string::npos);  // Heartbeat
}

TEST(FIXMessageBuilderTest, AddFields) {
    FIXMessageBuilder builder("ZEPTO", "SERVER");

    builder.add_field(55, "AAPL");  // Symbol
    builder.add_field(44, 150.50);  // Price
    builder.add_field(38, int64_t{100});     // OrderQty

    std::string msg = builder.build('D');  // New Order Single

    EXPECT_TRUE(msg.find("55=AAPL") != std::string::npos);
    EXPECT_TRUE(msg.find("44=150.50") != std::string::npos);
    EXPECT_TRUE(msg.find("38=100") != std::string::npos);
}

// ============================================================================
// Performance Tests
// ============================================================================
TEST(FIXParserPerformanceTest, ParseSpeed) {
    FIXParser parser;
    MockSymbolMapper mapper;

    const char* msg = "8=FIX.4.4\x01" "9=100\x01" "35=8\x01"
                      "55=AAPL\x01" "31=150.50\x01" "32=100\x01"
                      "54=1\x01" "10=123\x01";

    size_t len = std::strlen(msg);

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        parser.parse(msg, len);
    }

    // Benchmark
    auto start = std::chrono::steady_clock::now();
    constexpr int ITERATIONS = 100000;

    for (int i = 0; i < ITERATIONS; ++i) {
        parser.parse(msg, len);
        Tick tick;
        parser.extract_tick(tick, &mapper);
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();
    double ns_per_msg = (elapsed * 1e9) / ITERATIONS;

    std::cout << "FIX Parser: " << ns_per_msg << " ns/message\n";
    std::cout << "Throughput: " << (ITERATIONS / elapsed / 1e6) << " M msg/sec\n";

    // 목표: < 1000ns per message
    EXPECT_LT(ns_per_msg, 1000.0);
}

// ============================================================================
// Stage B (devlog 084): Table-aware ingest setters
// ============================================================================
TEST(FIXParserStageB, TableIdSetterRoundTrip) {
    FIXParser p;
    EXPECT_EQ(p.table_id(), 0u);
    EXPECT_TRUE(p.table_name().empty());
    p.set_table_id(7);
    p.set_table_name("orders");
    EXPECT_EQ(p.table_id(), 7u);
    EXPECT_EQ(p.table_name(), "orders");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

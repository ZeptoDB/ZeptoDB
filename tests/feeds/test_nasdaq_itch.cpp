// ============================================================================
// ZeptoDB: NASDAQ ITCH Parser Unit Tests
// ============================================================================
#include "zeptodb/feeds/nasdaq_itch.h"
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
// Helper Functions
// ============================================================================
void write_uint16_be(uint8_t* buf, uint16_t value) {
    buf[0] = (value >> 8) & 0xFF;
    buf[1] = value & 0xFF;
}

void write_uint32_be(uint8_t* buf, uint32_t value) {
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8) & 0xFF;
    buf[3] = value & 0xFF;
}

void write_uint64_be(uint8_t* buf, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buf[i] = (value >> (56 - i * 8)) & 0xFF;
    }
}

// ============================================================================
// ITCH Parser Tests
// ============================================================================
class ITCHParserTest : public ::testing::Test {
protected:
    NASDAQITCHParser parser_;
    MockSymbolMapper mapper_;
};

TEST_F(ITCHParserTest, ParseAddOrder) {
    // Add Order (Type A)
    uint8_t packet[256];
    size_t offset = 0;

    // Header
    write_uint16_be(packet + offset, 37);  // length (36 + 1 for type)
    offset += 2;
    packet[offset++] = 'A';  // message type

    // Add Order fields
    write_uint16_be(packet + offset, 1);  // stock_locate
    offset += 2;
    write_uint16_be(packet + offset, 100);  // tracking_number
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);  // timestamp (ns)
    offset += 8;
    write_uint64_be(packet + offset, 12345);  // order_reference
    offset += 8;
    packet[offset++] = 'B';  // buy_sell_indicator (Buy)
    write_uint32_be(packet + offset, 100);  // shares
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);  // stock (padded)
    offset += 8;
    write_uint32_be(packet + offset, 1500000);  // price (150.00)
    offset += 4;

    ASSERT_TRUE(parser_.parse_packet(packet, offset));
    EXPECT_EQ(parser_.get_message_type(), ITCHMessageType::ADD_ORDER);

    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_EQ(tick.symbol_id, 1);  // AAPL
    EXPECT_EQ(tick.timestamp_ns, 1000000000ULL);
    EXPECT_DOUBLE_EQ(tick.price, 150.00);
    EXPECT_EQ(tick.volume, 100);
    EXPECT_EQ(tick.side, Side::BUY);
    EXPECT_EQ(tick.type, TickType::ORDER);
}

TEST_F(ITCHParserTest, ParseTrade) {
    // Trade (Type P)
    uint8_t packet[256];
    size_t offset = 0;

    // Header
    write_uint16_be(packet + offset, 45);  // length
    offset += 2;
    packet[offset++] = 'P';  // message type

    // Trade fields
    write_uint16_be(packet + offset, 1);  // stock_locate
    offset += 2;
    write_uint16_be(packet + offset, 100);  // tracking_number
    offset += 2;
    write_uint64_be(packet + offset, 2000000000ULL);  // timestamp
    offset += 8;
    write_uint64_be(packet + offset, 12345);  // order_reference
    offset += 8;
    packet[offset++] = 'S';  // buy_sell_indicator (Sell)
    write_uint32_be(packet + offset, 200);  // shares
    offset += 4;
    std::memcpy(packet + offset, "MSFT    ", 8);  // stock
    offset += 8;
    write_uint32_be(packet + offset, 2500000);  // price (250.00)
    offset += 4;
    write_uint64_be(packet + offset, 99999);  // match_number
    offset += 8;

    ASSERT_TRUE(parser_.parse_packet(packet, offset));
    EXPECT_EQ(parser_.get_message_type(), ITCHMessageType::TRADE);

    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_EQ(tick.symbol_id, 2);  // MSFT
    EXPECT_EQ(tick.timestamp_ns, 2000000000ULL);
    EXPECT_DOUBLE_EQ(tick.price, 250.00);
    EXPECT_EQ(tick.volume, 200);
    EXPECT_EQ(tick.side, Side::SELL);
    EXPECT_EQ(tick.type, TickType::TRADE);
}

TEST_F(ITCHParserTest, ParseOrderExecuted) {
    // Order Executed (Type E)
    uint8_t packet[256];
    size_t offset = 0;

    // Header
    write_uint16_be(packet + offset, 31);  // length
    offset += 2;
    packet[offset++] = 'E';  // message type

    // Order Executed fields
    write_uint16_be(packet + offset, 1);  // stock_locate
    offset += 2;
    write_uint16_be(packet + offset, 100);  // tracking_number
    offset += 2;
    write_uint64_be(packet + offset, 3000000000ULL);  // timestamp
    offset += 8;
    write_uint64_be(packet + offset, 12345);  // order_reference
    offset += 8;
    write_uint32_be(packet + offset, 50);  // executed_shares
    offset += 4;
    write_uint64_be(packet + offset, 88888);  // match_number
    offset += 8;

    ASSERT_TRUE(parser_.parse_packet(packet, offset));
    EXPECT_EQ(parser_.get_message_type(), ITCHMessageType::ORDER_EXECUTED);

    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_EQ(tick.timestamp_ns, 3000000000ULL);
    EXPECT_EQ(tick.volume, 50);
    EXPECT_EQ(tick.type, TickType::TRADE);
}

TEST_F(ITCHParserTest, BigEndianConversion) {
    NASDAQITCHParser parser;

    uint8_t buf[8];
    write_uint64_be(buf, 0x0102030405060708ULL);

    // 내부 헬퍼 함수 테스트를 위한 간단한 검증
    // (실제로는 private이므로 간접적으로 검증)
    uint8_t packet[256];
    write_uint16_be(packet, 4);
    packet[2] = 'S';  // System Event
    write_uint64_be(packet + 3, 123456789ULL);

    ASSERT_TRUE(parser.parse_packet(packet, 11));
}

TEST_F(ITCHParserTest, StockSymbolParsing) {
    uint8_t packet[256];
    size_t offset = 0;

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';

    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;

    // 심볼 테스트: 오른쪽 공백 제거
    std::memcpy(packet + offset, "TSLA    ", 8);  // 4글자 + 4공백
    offset += 8;
    write_uint32_be(packet + offset, 1000000);
    offset += 4;

    ASSERT_TRUE(parser_.parse_packet(packet, offset));
    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_EQ(tick.symbol_id, 3);  // TSLA (공백 제거됨)
}

TEST_F(ITCHParserTest, PriceParsing) {
    // ITCH price: 10000 단위
    // 1500000 = $150.00
    // 1234567 = $123.4567

    uint8_t packet[256];
    size_t offset = 0;

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';
    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);
    offset += 8;
    write_uint32_be(packet + offset, 1234567);  // $123.4567
    offset += 4;

    ASSERT_TRUE(parser_.parse_packet(packet, offset));
    Tick tick;
    ASSERT_TRUE(parser_.extract_tick(tick, &mapper_));

    EXPECT_DOUBLE_EQ(tick.price, 123.4567);
}

TEST_F(ITCHParserTest, InvalidMessageType) {
    uint8_t packet[10];
    write_uint16_be(packet, 5);
    packet[2] = 'Z';  // 존재하지 않는 타입

    ASSERT_TRUE(parser_.parse_packet(packet, 10));
    EXPECT_EQ(parser_.get_message_type(), static_cast<ITCHMessageType>('Z'));

    // 알 수 없는 타입은 extract_tick 실패
    Tick tick;
    EXPECT_FALSE(parser_.extract_tick(tick, &mapper_));
}

TEST_F(ITCHParserTest, TooShortPacket) {
    uint8_t packet[2];
    write_uint16_be(packet, 100);  // 길이는 100인데 실제는 2바이트

    EXPECT_FALSE(parser_.parse_packet(packet, 2));
    EXPECT_GT(parser_.get_error_count(), 0);
}

// ============================================================================
// Performance Tests
// ============================================================================
TEST(ITCHParserPerformanceTest, ParseSpeed) {
    NASDAQITCHParser parser;
    MockSymbolMapper mapper;

    // Add Order 패킷 생성
    uint8_t packet[256];
    size_t offset = 0;

    write_uint16_be(packet + offset, 37);
    offset += 2;
    packet[offset++] = 'A';
    write_uint16_be(packet + offset, 1);
    offset += 2;
    write_uint16_be(packet + offset, 100);
    offset += 2;
    write_uint64_be(packet + offset, 1000000000ULL);
    offset += 8;
    write_uint64_be(packet + offset, 12345);
    offset += 8;
    packet[offset++] = 'B';
    write_uint32_be(packet + offset, 100);
    offset += 4;
    std::memcpy(packet + offset, "AAPL    ", 8);
    offset += 8;
    write_uint32_be(packet + offset, 1500000);
    offset += 4;

    size_t packet_len = offset;

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        parser.parse_packet(packet, packet_len);
    }

    // Benchmark
    auto start = std::chrono::steady_clock::now();
    constexpr int ITERATIONS = 100000;

    for (int i = 0; i < ITERATIONS; ++i) {
        parser.parse_packet(packet, packet_len);
        Tick tick;
        parser.extract_tick(tick, &mapper);
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(end - start).count();
    double ns_per_msg = (elapsed * 1e9) / ITERATIONS;

    std::cout << "ITCH Parser: " << ns_per_msg << " ns/message\n";
    std::cout << "Throughput: " << (ITERATIONS / elapsed / 1e6) << " M msg/sec\n";

    // 목표: < 500ns per message (바이너리라 더 빠름)
    EXPECT_LT(ns_per_msg, 500.0);
}

// main provided by test_fix_parser.cpp

// ============================================================================
// Test: Tick Plant (Layer 2)
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"
#include <gtest/gtest.h>

using namespace zeptodb::ingestion;

TEST(TickPlant, IngestAndConsume) {
    TickPlant tp;

    TickMessage msg{};
    msg.symbol_id = 1;
    msg.price = 150000;  // 15.0000 fixed-point
    msg.volume = 100;
    msg.msg_type = 0; // Trade

    EXPECT_TRUE(tp.ingest(msg));
    EXPECT_EQ(tp.current_seq(), 1);

    auto consumed = tp.consume();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed->seq_num, 0);
    EXPECT_EQ(consumed->symbol_id, 1);
    EXPECT_EQ(consumed->price, 150000);
    EXPECT_GT(consumed->recv_ts, 0);
}

TEST(TickPlant, FIFOOrdering) {
    TickPlant tp;

    for (int i = 0; i < 1000; ++i) {
        TickMessage msg{};
        msg.symbol_id = static_cast<uint32_t>(i % 10);
        msg.price = static_cast<int64_t>(i * 100);
        msg.volume = i;
        EXPECT_TRUE(tp.ingest(msg));
    }

    // Consume and verify monotonic sequence
    zeptodb::SeqNum last_seq = 0;
    bool first = true;
    for (int i = 0; i < 1000; ++i) {
        auto consumed = tp.consume();
        ASSERT_TRUE(consumed.has_value());
        if (!first) {
            EXPECT_GT(consumed->seq_num, last_seq);
        }
        last_seq = consumed->seq_num;
        first = false;
    }
}

TEST(TickPlant, EmptyConsume) {
    TickPlant tp;
    auto result = tp.consume();
    EXPECT_FALSE(result.has_value());
}

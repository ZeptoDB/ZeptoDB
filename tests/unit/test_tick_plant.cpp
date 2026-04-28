// ============================================================================
// Test: Tick Plant (Layer 2)
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/core/pipeline.h"
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

// ============================================================================
// Phase 1 ingest scale-out (devlog 102)
//   - drain_threads sentinel (0 = auto, >0 = honored exactly)
//   - ring_buffer_capacity configurable, power-of-two enforced
// ============================================================================

TEST(IngestPhase1DrainThreads, SentinelZeroAutoAtLeastTwo) {
    zeptodb::core::PipelineConfig cfg;
    cfg.drain_threads = 0;   // sentinel — auto
    zeptodb::core::ZeptoPipeline pipeline(cfg);
    pipeline.start();
    EXPECT_GE(pipeline.drain_thread_count(), 2u);
    pipeline.stop();
    EXPECT_EQ(pipeline.drain_thread_count(), 0u);
}

TEST(IngestPhase1DrainThreads, ExplicitValueHonoredExactly) {
    zeptodb::core::PipelineConfig cfg;
    cfg.drain_threads = 4;
    zeptodb::core::ZeptoPipeline pipeline(cfg);
    pipeline.start();
    EXPECT_EQ(pipeline.drain_thread_count(), 4u);
    pipeline.stop();
}

TEST(IngestPhase1RingCapacity, DefaultIs65536) {
    zeptodb::core::PipelineConfig cfg;  // ring_buffer_capacity defaults to 0 → engine default
    zeptodb::core::ZeptoPipeline pipeline(cfg);
    EXPECT_EQ(pipeline.tick_plant().capacity(), 65536u);
}

TEST(IngestPhase1RingCapacity, CustomPowerOfTwoHonored) {
    zeptodb::core::PipelineConfig cfg;
    cfg.ring_buffer_capacity = 262144;
    zeptodb::core::ZeptoPipeline pipeline(cfg);
    EXPECT_EQ(pipeline.tick_plant().capacity(), 262144u);
}

TEST(IngestPhase1RingCapacity, NonPowerOfTwoRejected) {
    zeptodb::core::PipelineConfig cfg;
    cfg.ring_buffer_capacity = 100000;  // not power of two
    EXPECT_THROW({ zeptodb::core::ZeptoPipeline pipeline(cfg); }, std::invalid_argument);
}

TEST(IngestPhase1RingCapacity, BelowRangeRejected) {
    zeptodb::core::PipelineConfig cfg;
    cfg.ring_buffer_capacity = 2048;  // power of two but below 4096 floor
    EXPECT_THROW({ zeptodb::core::ZeptoPipeline pipeline(cfg); }, std::invalid_argument);
}

TEST(IngestPhase1RingCapacity, AboveRangeRejected) {
    zeptodb::core::PipelineConfig cfg;
    cfg.ring_buffer_capacity = 33554432;  // 32 Mi — above 16 Mi ceiling
    EXPECT_THROW({ zeptodb::core::ZeptoPipeline pipeline(cfg); }, std::invalid_argument);
}

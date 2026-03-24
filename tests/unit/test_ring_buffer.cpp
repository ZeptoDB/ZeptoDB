// ============================================================================
// Test: MPMC Ring Buffer (Layer 2)
// ============================================================================

#include "zeptodb/ingestion/ring_buffer.h"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>

using namespace zeptodb::ingestion;

TEST(MPMCRingBuffer, BasicPushPop) {
    MPMCRingBuffer<int64_t, 1024> rb;

    EXPECT_TRUE(rb.try_push(42));
    EXPECT_TRUE(rb.try_push(99));

    auto v1 = rb.try_pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 42);

    auto v2 = rb.try_pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 99);

    auto v3 = rb.try_pop();
    EXPECT_FALSE(v3.has_value());
}

TEST(MPMCRingBuffer, MultiProducerSingleConsumer) {
    MPMCRingBuffer<int64_t, 65536> rb;

    constexpr int PRODUCERS = 4;
    constexpr int ITEMS_PER = 10000;

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&rb, p]() {
            for (int i = 0; i < ITEMS_PER; ++i) {
                while (!rb.try_push(p * ITEMS_PER + i)) {
                    // spin
                }
            }
        });
    }

    std::atomic<int> consumed{0};
    std::thread consumer([&]() {
        while (consumed.load() < PRODUCERS * ITEMS_PER) {
            if (rb.try_pop().has_value()) {
                consumed.fetch_add(1);
            }
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), PRODUCERS * ITEMS_PER);
}

TEST(MPMCRingBuffer, ApproxSize) {
    MPMCRingBuffer<int, 1024> rb;

    for (int i = 0; i < 100; ++i) {
        rb.try_push(i);
    }
    EXPECT_GE(rb.approx_size(), 90);  // approximate

    for (int i = 0; i < 50; ++i) {
        rb.try_pop();
    }
    EXPECT_LE(rb.approx_size(), 60);
}

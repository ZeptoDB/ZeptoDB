// ============================================================================
// Test: Column Store (Layer 1)
// ============================================================================

#include "zeptodb/storage/column_store.h"
#include <gtest/gtest.h>

using namespace zeptodb::storage;

class ColumnStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ArenaConfig config{.total_size = 4 * 1024 * 1024, .use_hugepages = false};
        arena_ = std::make_unique<ArenaAllocator>(config);
    }
    std::unique_ptr<ArenaAllocator> arena_;
};

TEST_F(ColumnStoreTest, CreateAndAppend) {
    ColumnVector col("price", ColumnType::INT64, *arena_);

    EXPECT_EQ(col.name(), "price");
    EXPECT_EQ(col.type(), ColumnType::INT64);
    EXPECT_EQ(col.size(), 0);

    EXPECT_TRUE(col.append<int64_t>(15000));
    EXPECT_TRUE(col.append<int64_t>(15050));
    EXPECT_EQ(col.size(), 2);

    auto span = col.as_span<int64_t>();
    EXPECT_EQ(span[0], 15000);
    EXPECT_EQ(span[1], 15050);
}

TEST_F(ColumnStoreTest, BatchAppend) {
    ColumnVector col("volume", ColumnType::INT64, *arena_);

    std::vector<int64_t> batch(8192);
    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i] = static_cast<int64_t>(i * 10);
    }

    EXPECT_TRUE(col.append_batch(batch.data(), batch.size()));
    EXPECT_EQ(col.size(), 8192);

    auto span = col.as_span<int64_t>();
    EXPECT_EQ(span[0], 0);
    EXPECT_EQ(span[4096], 40960);
    EXPECT_EQ(span[8191], 81910);
}

TEST_F(ColumnStoreTest, MultipleTypes) {
    ColumnVector prices("price", ColumnType::INT64, *arena_);
    ColumnVector symbols("symbol", ColumnType::SYMBOL, *arena_);
    ColumnVector flags("active", ColumnType::BOOL, *arena_);

    prices.append<int64_t>(50000);
    symbols.append<uint32_t>(42);
    flags.append<uint8_t>(1);

    EXPECT_EQ(prices.as_span<int64_t>()[0], 50000);
    EXPECT_EQ(symbols.as_span<uint32_t>()[0], 42);
    EXPECT_EQ(flags.as_span<uint8_t>()[0], 1);
}

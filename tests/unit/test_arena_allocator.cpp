// ============================================================================
// Test: Arena Allocator (Layer 1)
// ============================================================================

#include "zeptodb/storage/arena_allocator.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace zeptodb::storage;

TEST(ArenaAllocator, BasicAllocation) {
    ArenaConfig config{.total_size = 1024 * 1024, .use_hugepages = false};
    ArenaAllocator arena(config);

    EXPECT_EQ(arena.total_capacity(), 1024 * 1024);
    EXPECT_EQ(arena.used_bytes(), 0);

    void* p1 = arena.allocate(256);
    ASSERT_NE(p1, nullptr);
    EXPECT_GT(arena.used_bytes(), 0);

    // Alignment check (cache-line)
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % zeptodb::CACHE_LINE_SIZE, 0);
}

TEST(ArenaAllocator, ArrayAllocation) {
    ArenaConfig config{.total_size = 4 * 1024 * 1024, .use_hugepages = false};
    ArenaAllocator arena(config);

    int64_t* prices = arena.allocate_array<int64_t>(8192);
    ASSERT_NE(prices, nullptr);

    // Write and read back
    for (size_t i = 0; i < 8192; ++i) {
        prices[i] = static_cast<int64_t>(i * 100);
    }
    EXPECT_EQ(prices[0], 0);
    EXPECT_EQ(prices[8191], 819100);
}

TEST(ArenaAllocator, ConcurrentAllocation) {
    ArenaConfig config{.total_size = 64 * 1024 * 1024, .use_hugepages = false};
    ArenaAllocator arena(config);

    constexpr int NUM_THREADS = 4;
    constexpr int ALLOCS_PER_THREAD = 1000;
    constexpr size_t ALLOC_SIZE = 1024;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ALLOCS_PER_THREAD; ++i) {
                void* p = arena.allocate(ALLOC_SIZE);
                if (p) success_count.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(success_count.load(), NUM_THREADS * ALLOCS_PER_THREAD);
}

TEST(ArenaAllocator, Reset) {
    ArenaConfig config{.total_size = 1024 * 1024, .use_hugepages = false};
    ArenaAllocator arena(config);

    arena.allocate(512 * 1024);
    EXPECT_GT(arena.used_bytes(), 0);

    arena.reset();
    EXPECT_EQ(arena.used_bytes(), 0);
}

TEST(ArenaAllocator, Exhaustion) {
    ArenaConfig config{.total_size = 4096, .use_hugepages = false};
    ArenaAllocator arena(config);

    // Allocate the whole thing
    void* p1 = arena.allocate(4096);
    ASSERT_NE(p1, nullptr);

    // Next should fail
    void* p2 = arena.allocate(1);
    EXPECT_EQ(p2, nullptr);
}

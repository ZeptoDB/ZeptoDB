// ============================================================================
// ZeptoDB: FlatHashMap 단위 테스트
// ============================================================================

#include "zeptodb/execution/flat_hash_map.h"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <unordered_set>

using namespace zeptodb::execution;

// ============================================================================
// 기본 동작
// ============================================================================

TEST(FlatHashMap, InsertAndFind) {
    FlatHashMap<int64_t> map;
    map[42] = 100;
    map[99] = 200;

    auto* v1 = map.find(42);
    auto* v2 = map.find(99);
    auto* v3 = map.find(0);

    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v3, nullptr);
    EXPECT_EQ(*v1, 100);
    EXPECT_EQ(*v2, 200);
    EXPECT_EQ(map.size(), 2u);
}

TEST(FlatHashMap, OverwriteSameKey) {
    FlatHashMap<int64_t> map;
    map[10] = 1;
    map[10] = 2;

    EXPECT_EQ(map.size(), 1u);
    EXPECT_EQ(*map.find(10), 2);
}

TEST(FlatHashMap, EmptyMap) {
    FlatHashMap<int64_t> map;
    EXPECT_EQ(map.find(0), nullptr);
    EXPECT_EQ(map.size(), 0u);
}

TEST(FlatHashMap, NegativeKeys) {
    FlatHashMap<int64_t> map;
    map[-1] = 10;
    map[-100] = 20;
    map[INT64_MIN] = 30;

    EXPECT_EQ(*map.find(-1), 10);
    EXPECT_EQ(*map.find(-100), 20);
    EXPECT_EQ(*map.find(INT64_MIN), 30);
}

// ============================================================================
// Vector value (join 패턴)
// ============================================================================

TEST(FlatHashMap, VectorValue) {
    FlatHashMap<std::vector<int64_t>> map(8);
    map[1].push_back(10);
    map[1].push_back(20);
    map[2].push_back(30);

    auto* v = map.find(1);
    ASSERT_NE(v, nullptr);
    ASSERT_EQ(v->size(), 2u);
    EXPECT_EQ((*v)[0], 10);
    EXPECT_EQ((*v)[1], 20);

    EXPECT_EQ(map.find(2)->size(), 1u);
    EXPECT_EQ(map.find(3), nullptr);
}

// ============================================================================
// 대규모 + 정확성
// ============================================================================

TEST(FlatHashMap, LargeInsertAndLookup) {
    const int N = 10000;
    FlatHashMap<int64_t> map(N);

    for (int i = 0; i < N; ++i) {
        map[static_cast<int64_t>(i)] = static_cast<int64_t>(i * 3);
    }

    EXPECT_EQ(map.size(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        auto* v = map.find(static_cast<int64_t>(i));
        ASSERT_NE(v, nullptr) << "key=" << i;
        EXPECT_EQ(*v, static_cast<int64_t>(i * 3));
    }

    // 존재하지 않는 키
    EXPECT_EQ(map.find(static_cast<int64_t>(N)), nullptr);
    EXPECT_EQ(map.find(-1), nullptr);
}

// ============================================================================
// for_each 순회
// ============================================================================

TEST(FlatHashMap, ForEach) {
    FlatHashMap<int64_t> map;
    map[1] = 10;
    map[2] = 20;
    map[3] = 30;

    std::unordered_set<int64_t> seen_keys;
    int64_t sum = 0;
    map.for_each([&](int64_t key, int64_t& val) {
        seen_keys.insert(key);
        sum += val;
    });

    EXPECT_EQ(seen_keys.size(), 3u);
    EXPECT_TRUE(seen_keys.count(1));
    EXPECT_TRUE(seen_keys.count(2));
    EXPECT_TRUE(seen_keys.count(3));
    EXPECT_EQ(sum, 60);
}

// ============================================================================
// 해시 충돌 시나리오 (같은 하위 비트를 가진 키들)
// ============================================================================

TEST(FlatHashMap, CollisionHandling) {
    // 작은 capacity로 충돌 유발
    FlatHashMap<int64_t> map(4);

    // 16개 키 삽입 → 내부 capacity 32 (load factor 50%)
    for (int i = 0; i < 16; ++i) {
        map[static_cast<int64_t>(i)] = static_cast<int64_t>(i * 10);
    }

    for (int i = 0; i < 16; ++i) {
        auto* v = map.find(static_cast<int64_t>(i));
        ASSERT_NE(v, nullptr) << "key=" << i;
        EXPECT_EQ(*v, static_cast<int64_t>(i * 10));
    }
}

// ============================================================================
// const find
// ============================================================================

TEST(FlatHashMap, ConstFind) {
    FlatHashMap<int64_t> map;
    map[5] = 50;

    const auto& cmap = map;
    const auto* v = cmap.find(5);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 50);
    EXPECT_EQ(cmap.find(6), nullptr);
}

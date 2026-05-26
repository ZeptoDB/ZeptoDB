// ============================================================================
// ParquetWriter Unit Tests (devlog 118)
// ============================================================================
// Round-trip a tiny in-memory Partition → ParquetWriter → ParquetReader.
// Skip when Arrow/Parquet are not built into the binary.
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/storage/parquet_writer.h"
#include "zeptodb/storage/parquet_reader.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/storage/arena_allocator.h"

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

using namespace zeptodb;
using namespace zeptodb::storage;

// ============================================================================
// Build-time Parquet availability flag is reported correctly.
// ============================================================================
TEST(ParquetWriterTest, Available) {
    const bool av = ParquetWriter::parquet_available();
#if ZEPTO_PARQUET_AVAILABLE
    EXPECT_TRUE(av);
#else
    EXPECT_FALSE(av);
#endif
    SUCCEED() << "ParquetWriter::parquet_available() = " << (av ? "true" : "false");
}

// ============================================================================
// parquet_filename(symbol, hour) → "<sym>_<hour>.parquet"
// (free function in parquet_writer.h — covered for documentation parity.)
// ============================================================================
TEST(ParquetWriterTest, Filename) {
    EXPECT_EQ(parquet_filename(/*symbol=*/42, /*hour=*/12345),
              "42_12345.parquet");
    EXPECT_EQ(parquet_filename(0, 0), "0_0.parquet");
}

// ============================================================================
// Round-trip a tiny Partition (3 columns, 4 rows) through Parquet.
// Skipped when Arrow/Parquet are not compiled in.
// ============================================================================
TEST(ParquetWriterTest, RoundTrip_SmallPartition) {
    if (!ParquetWriter::parquet_available()) {
        GTEST_SKIP() << "Arrow/Parquet not available in this build";
    }

    // --- build a 4-row partition with INT64, FLOAT64, TIMESTAMP_NS ---
    auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
        .total_size = 1 << 20,  // 1 MiB
        .use_hugepages = false,
    });
    PartitionKey key{0, /*symbol=*/42, /*hour_epoch=*/100};
    Partition part(key, std::move(arena));

    auto& price = part.add_column("price",     ColumnType::INT64);
    auto& ratio = part.add_column("ratio",     ColumnType::FLOAT64);
    auto& ts    = part.add_column("timestamp", ColumnType::TIMESTAMP_NS);

    constexpr size_t N = 4;
    for (size_t i = 0; i < N; ++i) {
        price.append<int64_t>(static_cast<int64_t>(1000 + i));
        ratio.append<double>(0.5 + static_cast<double>(i));
        ts.append<int64_t>(static_cast<int64_t>(1'000'000 * (i + 1)));
    }
    part.seal();

    // --- write it out to a per-test temp dir ---
    const auto out_dir = fs::temp_directory_path() /
        ("zepto_parquet_rt_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(out_dir);

    ParquetWriter writer{};
    const auto out_path = writer.flush_to_file(part, out_dir.string());
    ASSERT_FALSE(out_path.empty()) << "ParquetWriter::flush_to_file failed";
    ASSERT_TRUE(fs::exists(out_path));
    EXPECT_GT(fs::file_size(out_path), 0u);

    // --- read it back via ParquetReader and check shape ---
    ParquetReader reader;
    auto res = reader.read_file(out_path, /*limit=*/0);
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.column_names.size(), 3u);
    EXPECT_EQ(res.total_rows, N);

    // ParquetReader::read_file casts non-int64 columns to 0; only assert a
    // value on the int64 "price" column to keep the test reader-agnostic.
    int price_col = -1;
    for (size_t i = 0; i < res.column_names.size(); ++i) {
        if (res.column_names[i] == "price") price_col = static_cast<int>(i);
    }
    ASSERT_GE(price_col, 0);
    ASSERT_FALSE(res.rows.empty());
    EXPECT_EQ(res.rows.front()[price_col], 1000);
    EXPECT_EQ(res.rows.back ()[price_col], static_cast<int64_t>(1000 + N - 1));

    // --- cleanup ---
    std::error_code ec;
    fs::remove_all(out_dir, ec);
}

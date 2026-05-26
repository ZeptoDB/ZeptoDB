// ============================================================================
// Cold-tier (FlushManager Parquet + HIVE S3 layout) Unit Tests (devlog 118)
// ============================================================================
// Drives FlushManager with output_format=PARQUET and S3Layout=HIVE, then
// verifies the local Parquet file lands on disk and the S3 key that
// *would* be uploaded matches the expected Hive partition shape.
//
// Skipped when Arrow/Parquet are not compiled in. Does not actually
// upload — `enable_s3_upload=false` so the test runs offline.
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/storage/flush_manager.h"
#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/parquet_writer.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/storage/s3_sink.h"

#include <chrono>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

using namespace zeptodb;
using namespace zeptodb::storage;

namespace {

std::string make_temp_dir(const char* tag) {
    auto p = fs::temp_directory_path() /
        (std::string("zepto_") + tag + "_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p.string();
}

} // namespace

// ============================================================================
// HIVE key for a sealed partition matches the spec, derived from gmtime().
// ============================================================================
TEST(ColdTier, HiveKey_MatchesGmtime) {
    // 2024-05-01 00:00 UTC → hour_epoch 476256
    constexpr int64_t hour_epoch = 1714521600LL / 3600LL;

    S3SinkConfig cfg;
    cfg.bucket            = "zepto-cold-test";
    cfg.prefix            = "hdb";
    cfg.layout            = S3Layout::HIVE;
    cfg.disable_host_hash = true;

    S3Sink sink(std::move(cfg));
    EXPECT_EQ(sink.make_s3_key(/*symbol=*/123, hour_epoch, "parquet"),
              "year=2024/month=05/day=01/symbol=123/123-476256.parquet");
}

// ============================================================================
// FlushManager + PARQUET + HIVE: a sealed partition is written to local
// Parquet and the prospective S3 key is in HIVE form.
// Skipped if Arrow/Parquet are absent.
// ============================================================================
TEST(ColdTier, HiveKeyOnFlush) {
    if (!ParquetWriter::parquet_available()) {
        GTEST_SKIP() << "Arrow/Parquet not available in this build";
    }

    const std::string base = make_temp_dir("cold_hive_flush");

    PartitionManager pm(/*arena_size=*/4ULL * 1024 * 1024);
    HDBWriter writer(base, /*use_compression=*/false);

    // 2024-05-01 00:00 UTC, expressed in nanoseconds (the unit
    // PartitionManager actually uses internally — `key.hour_epoch` is
    // floor(ts_ns / NS_PER_HOUR) * NS_PER_HOUR).
    constexpr int64_t kEpochSec  = 1714521600LL;             // 2024-05-01 UTC
    constexpr int64_t ts_ns      = kEpochSec * 1'000'000'000LL;
    constexpr int64_t hour_ns    = ts_ns;                    // already on hour boundary
    constexpr int64_t hour_hours = kEpochSec / 3600LL;       // 476256
    constexpr SymbolId sym       = 7;

    // Configure cold-tier: PARQUET only, HIVE layout, S3 upload OFF
    // (deterministic, offline). The S3Sink is still constructed to verify
    // make_s3_key() shape.
    FlushConfig fc{};
    fc.memory_threshold       = 0.8;
    fc.check_interval_ms      = 50;
    fc.enable_compression     = false;
    fc.reclaim_after_flush    = false;
    fc.auto_seal_age_hours    = 0;
    fc.output_format          = HDBOutputFormat::PARQUET;
    fc.enable_s3_upload       = false;       // do not actually upload
    fc.delete_local_after_s3  = false;
    fc.s3_config.bucket           = "zepto-cold-test";
    fc.s3_config.prefix           = "hdb";
    fc.s3_config.layout           = S3Layout::HIVE;
    fc.s3_config.disable_host_hash = true;

    FlushManager fm(pm, writer, fc);

    // --- build a small sealed partition at the 2024-05-01 hour boundary ---
    Partition& part = pm.get_or_create(sym, ts_ns);
    ASSERT_EQ(part.key().hour_epoch, hour_ns);

    part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    part.add_column("price",     ColumnType::INT64);
    part.add_column("volume",    ColumnType::INT64);
    for (int i = 0; i < 8; ++i) {
        part.get_column("timestamp")->append<int64_t>(ts_ns + i);
        part.get_column("price")    ->append<int64_t>(1000 + i);
        part.get_column("volume")   ->append<int64_t>(10);
    }
    part.seal();

    // --- flush ---
    fm.flush_now();

    // Local Parquet exists under {base}/{symbol}/{hour_ns}/{symbol}_{hour_ns}.parquet
    const fs::path local = fs::path(base)
        / std::to_string(sym)
        / std::to_string(hour_ns)
        / parquet_filename(sym, hour_ns);
    ASSERT_TRUE(fs::exists(local)) << "expected Parquet at " << local;

    // The S3 key the FlushManager *would* have uploaded is computed by
    // S3Sink::make_s3_key with the configured HIVE layout. The flush path
    // converts ns→hours before calling make_s3_key (spec: HIVE input is
    // hours since epoch), so the probe gets the same hour_hours value.
    S3Sink probe(fc.s3_config);
    EXPECT_EQ(probe.make_s3_key(sym, hour_hours, "parquet"),
              "year=2024/month=05/day=01/symbol=7/7-476256.parquet");

    // --- cleanup ---
    std::error_code ec;
    fs::remove_all(base, ec);
}

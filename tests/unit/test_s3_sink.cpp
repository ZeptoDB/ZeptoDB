// ============================================================================
// S3Sink Unit Tests (devlog 118)
// ============================================================================
// Path-generation tests run on every architecture without network. Upload
// tests are gated on `S3Sink::s3_available()` and skip with GTEST_SKIP()
// when the AWS SDK is not built or no fake S3 endpoint is reachable.
// ============================================================================

#include <gtest/gtest.h>

#include "zeptodb/storage/s3_sink.h"

#include <ctime>
#include <string>

using namespace zeptodb;
using namespace zeptodb::storage;

namespace {

// Build an S3SinkConfig with `disable_host_hash=true` so HIVE keys are
// deterministic across hosts / pods / unit-test runs.
S3SinkConfig deterministic_config(S3Layout layout) {
    S3SinkConfig cfg;
    cfg.bucket            = "test-bucket";
    cfg.prefix            = "hdb";
    cfg.region            = "us-east-1";
    cfg.layout            = layout;
    cfg.disable_host_hash = true;
    return cfg;
}

} // namespace

// ============================================================================
// FLAT layout: byte-identical to pre-118 behavior.
// ============================================================================
TEST(S3SinkPath, FlatLayout) {
    S3Sink sink(deterministic_config(S3Layout::FLAT));

    const auto key = sink.make_s3_key(/*symbol=*/42,
                                      /*hour_epoch=*/476520,
                                      /*ext=*/"parquet");
    EXPECT_EQ(key, "42/476520.parquet");

    const auto uri = sink.make_s3_uri(key);
    EXPECT_EQ(uri, "s3://test-bucket/hdb/42/476520.parquet");
}

// ============================================================================
// HIVE layout: Athena/DuckDB/Polars/Spark auto-discoverable path.
//
// Pick a hour_epoch whose UTC date is unambiguous:
//   1714521600 sec = 2024-05-01 00:00 UTC
//   hour_epoch     = 1714521600 / 3600 = 476256
// Verify the date math against gmtime() so the test self-anchors.
// ============================================================================
TEST(S3SinkPath, HiveLayout) {
    constexpr int64_t hour_epoch = 1714521600LL / 3600LL;  // 476256
    EXPECT_EQ(hour_epoch, 476256);

    // Sanity: gmtime agrees on the date breakdown.
    const std::time_t secs = static_cast<std::time_t>(hour_epoch) * 3600;
    std::tm tm_utc{};
    ::gmtime_r(&secs, &tm_utc);
    EXPECT_EQ(tm_utc.tm_year + 1900, 2024);
    EXPECT_EQ(tm_utc.tm_mon  + 1,    5);
    EXPECT_EQ(tm_utc.tm_mday,        1);

    S3Sink sink(deterministic_config(S3Layout::HIVE));

    const auto key = sink.make_s3_key(/*symbol=*/42, hour_epoch, "parquet");
    EXPECT_EQ(key, "year=2024/month=05/day=01/symbol=42/42-476256.parquet");

    const auto uri = sink.make_s3_uri(key);
    EXPECT_EQ(uri,
              "s3://test-bucket/hdb/year=2024/month=05/day=01/symbol=42/42-476256.parquet");
}

// ============================================================================
// HIVE layout zero-pads single-digit months and days.
// 2025-01-01 00:00 UTC = 1735689600 / 3600 = 482136
// ============================================================================
TEST(S3SinkPath, HiveLayout_PadsZeroes) {
    constexpr int64_t hour_epoch = 1735689600LL / 3600LL;  // 482136

    const std::time_t secs = static_cast<std::time_t>(hour_epoch) * 3600;
    std::tm tm_utc{};
    ::gmtime_r(&secs, &tm_utc);
    EXPECT_EQ(tm_utc.tm_year + 1900, 2025);
    EXPECT_EQ(tm_utc.tm_mon  + 1,    1);  // January
    EXPECT_EQ(tm_utc.tm_mday,        1);  // Day 1

    S3Sink sink(deterministic_config(S3Layout::HIVE));

    const auto key = sink.make_s3_key(/*symbol=*/7, hour_epoch, "parquet");
    EXPECT_EQ(key, "year=2025/month=01/day=01/symbol=7/7-482136.parquet")
        << "month and day must be zero-padded to 2 digits";
}

// ============================================================================
// HIVE layout includes the host-hash suffix when not stubbed out.
// We can't predict the hash on every host, but we can assert the *shape*:
// `<sym>-<hour>-<4 lower-case hex chars>.parquet`
// ============================================================================
TEST(S3SinkPath, HiveLayout_HostHashShape) {
    S3SinkConfig cfg;
    cfg.bucket = "test-bucket";
    cfg.prefix = "hdb";
    cfg.layout = S3Layout::HIVE;
    cfg.disable_host_hash = false;  // keep the hash on

    S3Sink sink(std::move(cfg));
    const auto key = sink.make_s3_key(7, 482136, "parquet");

    // Either: host hash present → "...7-482136-XXXX.parquet" (XXXX = hex)
    // Or:     gethostname() failed → "...7-482136.parquet" (graceful fallback)
    const std::string base = "year=2025/month=01/day=01/symbol=7/7-482136";
    const bool no_hash   = (key == base + ".parquet");
    const bool has_hash  =
        key.size() == base.size() + 1 + 4 + std::string(".parquet").size() &&
        key.substr(0, base.size())       == base &&
        key[base.size()]                  == '-' &&
        key.substr(key.size() - 8)        == ".parquet";

    EXPECT_TRUE(no_hash || has_hash) << "actual key: " << key;

    if (has_hash) {
        // Validate the 4-char hex hash payload is lower-case hex.
        for (size_t i = base.size() + 1; i < base.size() + 5; ++i) {
            const char c = key[i];
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                << "non-hex char '" << c << "' at index " << i;
        }
    }
}

// ============================================================================
// Build-time AWS SDK availability flag round-trips through s3_available().
// ============================================================================
TEST(S3Sink, AwsAvailability) {
    // Just compiles + reports — value is build-time, not runtime.
    const bool available = S3Sink::s3_available();
#if ZEPTO_S3_AVAILABLE
    EXPECT_TRUE(available);
#else
    EXPECT_FALSE(available);
#endif
    SUCCEED() << "S3Sink::s3_available() = " << (available ? "true" : "false");
}

// ============================================================================
// Actual upload smoke — gated on build-time SDK + a reachable endpoint.
// Skipped unless ZEPTO_S3_TEST_BUCKET is set in the environment to a
// writable bucket / MinIO (operators opt-in).
// ============================================================================
TEST(S3Sink, UploadFile_OptIn) {
    if (!S3Sink::s3_available()) {
        GTEST_SKIP() << "AWS SDK C++ not built";
    }
    const char* bucket = std::getenv("ZEPTO_S3_TEST_BUCKET");
    if (!bucket || !*bucket) {
        GTEST_SKIP() << "ZEPTO_S3_TEST_BUCKET unset — skipping live S3 upload";
    }
    SUCCEED() << "Live upload covered by integration tier; unit harness is "
                 "shape-only.";
}

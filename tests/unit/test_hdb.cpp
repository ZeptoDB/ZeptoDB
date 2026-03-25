// ============================================================================
// HDB Unit Tests: HDBWriter, HDBReader, FlushManager, Tiered Query
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>
#include <thread>
#include <chrono>

#include <spdlog/spdlog.h>

#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/hdb_reader.h"
#include "zeptodb/storage/flush_manager.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/common/logger.h"

namespace fs = std::filesystem;

using namespace zeptodb;
using namespace zeptodb::storage;
using namespace zeptodb::core;

// ============================================================================
// 테스트 픽스처: 임시 디렉토리 자동 정리
// ============================================================================
class HDBTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 전역 로거 초기화 (이미 있으면 스킵)
        if (!spdlog::get("zepto_test")) {
            Logger::init("zepto_test", spdlog::level::warn);
        }

        // 임시 디렉토리 생성
        temp_dir_ = fs::temp_directory_path() / ("zepto_hdb_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch().count()));
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        // 임시 디렉토리 정리
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    /// 테스트용 파티션 생성 (n_rows 행, 단순 데이터)
    std::unique_ptr<Partition> make_partition(
        SymbolId symbol_id, int64_t hour_epoch, size_t n_rows,
        int64_t base_price = 100'0000LL,  // 100.0000
        int64_t base_volume = 1000LL
    ) {
        auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
            .total_size = 4ULL * 1024 * 1024,  // 4MB
            .use_hugepages = false,
        });

        PartitionKey key{symbol_id, hour_epoch};
        auto part = std::make_unique<Partition>(key, std::move(arena));

        part->add_column("timestamp",  ColumnType::TIMESTAMP_NS);
        part->add_column("price",      ColumnType::INT64);
        part->add_column("volume",     ColumnType::INT64);
        part->add_column("msg_type",   ColumnType::INT32);

        for (size_t i = 0; i < n_rows; ++i) {
            const int64_t ts = hour_epoch + static_cast<int64_t>(i) * 1'000'000;
            part->get_column("timestamp")->append<int64_t>(ts);
            part->get_column("price")->append<int64_t>(base_price + static_cast<int64_t>(i));
            part->get_column("volume")->append<int64_t>(base_volume + static_cast<int64_t>(i % 100));
            part->get_column("msg_type")->append<int32_t>(1);
        }

        part->seal();
        return part;
    }

    std::string temp_dir_;
};

// ============================================================================
// 1. HDBWriter: 파티션 직렬화 기본 테스트
// ============================================================================
TEST_F(HDBTest, WritePartition_CreatesFiles) {
    HDBWriter writer(temp_dir_, false);  // 압축 없음

    const SymbolId symbol   = 42;
    const int64_t  hour     = 1'700'000'000'000'000'000LL;
    const size_t   n_rows   = 1000;

    auto part = make_partition(symbol, hour, n_rows);

    const size_t bytes_written = writer.flush_partition(*part);
    EXPECT_GT(bytes_written, 0u) << "플러시 결과가 0 바이트여선 안 됨";
    EXPECT_EQ(writer.partitions_flushed(), 1u);
    EXPECT_EQ(writer.total_bytes_written(), bytes_written);

    // 파일 존재 확인
    const std::string dir = temp_dir_ + "/" +
                            std::to_string(symbol) + "/" +
                            std::to_string(hour);
    EXPECT_TRUE(fs::exists(dir + "/timestamp.bin"));
    EXPECT_TRUE(fs::exists(dir + "/price.bin"));
    EXPECT_TRUE(fs::exists(dir + "/volume.bin"));
    EXPECT_TRUE(fs::exists(dir + "/msg_type.bin"));
}

// ============================================================================
// 2. HDBWriter + HDBReader: 왕복(Round-trip) 데이터 무결성 테스트
// ============================================================================
TEST_F(HDBTest, WriteReadRoundTrip_DataIntegrity) {
    const SymbolId symbol  = 7;
    const int64_t  hour    = 3600LL * 1'000'000'000LL;  // 1시간 epoch
    const size_t   n_rows  = 5000;
    const int64_t  base_px = 50000'0000LL;

    // 쓰기
    {
        HDBWriter writer(temp_dir_, false);
        auto part = make_partition(symbol, hour, n_rows, base_px, 500);
        writer.flush_partition(*part);
    }

    // 읽기 + 검증
    HDBReader reader(temp_dir_);

    // timestamp 컬럼 검증
    {
        auto col = reader.read_column(symbol, hour, "timestamp");
        ASSERT_TRUE(col.valid()) << "timestamp 컬럼 읽기 실패";
        EXPECT_EQ(col.num_rows, n_rows);
        EXPECT_EQ(col.type, ColumnType::TIMESTAMP_NS);

        const auto span = col.as_span<int64_t>();
        EXPECT_EQ(span[0], hour);
        EXPECT_EQ(span[1], hour + 1'000'000LL);
        EXPECT_EQ(span[n_rows - 1], hour + static_cast<int64_t>(n_rows - 1) * 1'000'000LL);
    }

    // price 컬럼 검증
    {
        auto col = reader.read_column(symbol, hour, "price");
        ASSERT_TRUE(col.valid());
        EXPECT_EQ(col.num_rows, n_rows);

        const auto span = col.as_span<int64_t>();
        for (size_t i = 0; i < n_rows; ++i) {
            EXPECT_EQ(span[i], base_px + static_cast<int64_t>(i))
                << "price[" << i << "] 불일치";
        }
    }

    // volume 컬럼 검증
    {
        auto col = reader.read_column(symbol, hour, "volume");
        ASSERT_TRUE(col.valid());
        EXPECT_EQ(col.num_rows, n_rows);

        const auto span = col.as_span<int64_t>();
        for (size_t i = 0; i < n_rows; ++i) {
            EXPECT_EQ(span[i], 500LL + static_cast<int64_t>(i % 100));
        }
    }
}

// ============================================================================
// 3. LZ4 압축 왕복 테스트
// ============================================================================
TEST_F(HDBTest, WriteReadRoundTrip_LZ4Compression) {
    if (!HDBWriter::lz4_available()) {
        GTEST_SKIP() << "LZ4 라이브러리 없음 — 압축 테스트 스킵";
    }

    const SymbolId symbol = 99;
    const int64_t  hour   = 7200LL * 1'000'000'000LL;
    const size_t   n_rows = 10000;
    const int64_t  base_px = 12345'0000LL;

    // LZ4 압축으로 쓰기
    size_t compressed_bytes = 0;
    {
        HDBWriter writer(temp_dir_, true);  // 압축 ON
        auto part = make_partition(symbol, hour, n_rows, base_px, 100);
        compressed_bytes = writer.flush_partition(*part);
        EXPECT_GT(compressed_bytes, 0u);
    }

    // 비압축 크기 비교 (압축이 효과적인지 확인)
    size_t raw_bytes = 0;
    const std::string raw_dir = temp_dir_ + "_raw";
    {
        HDBWriter writer(raw_dir, false);  // 압축 OFF
        auto part = make_partition(symbol, hour, n_rows, base_px, 100);
        raw_bytes = writer.flush_partition(*part);
    }

    EXPECT_LT(compressed_bytes, raw_bytes)
        << "LZ4 압축이 오히려 커짐 (데이터에 따라 발생 가능)";

    // 압축된 파일 읽기 + 데이터 검증
    HDBReader reader(temp_dir_);
    auto col = reader.read_column(symbol, hour, "price");
    ASSERT_TRUE(col.valid()) << "LZ4 압축 price 컬럼 읽기 실패";
    EXPECT_EQ(col.num_rows, n_rows);

    const auto span = col.as_span<int64_t>();
    for (size_t i = 0; i < n_rows; ++i) {
        EXPECT_EQ(span[i], base_px + static_cast<int64_t>(i))
            << "LZ4 압축 해제 후 price[" << i << "] 불일치";
    }

    // 정리
    std::error_code ec;
    fs::remove_all(raw_dir, ec);
}

// ============================================================================
// 4. HDBReader: list_partitions 테스트
// ============================================================================
TEST_F(HDBTest, ListPartitions) {
    const SymbolId symbol = 3;
    HDBWriter writer(temp_dir_, false);

    // 3개의 파티션 기록
    const std::vector<int64_t> hours = {
        3600LL  * 1'000'000'000LL,
        7200LL  * 1'000'000'000LL,
        10800LL * 1'000'000'000LL,
    };

    for (const int64_t h : hours) {
        auto part = make_partition(symbol, h, 100);
        writer.flush_partition(*part);
    }

    HDBReader reader(temp_dir_);
    const auto partitions = reader.list_partitions(symbol);

    ASSERT_EQ(partitions.size(), 3u);
    EXPECT_EQ(partitions[0], hours[0]);
    EXPECT_EQ(partitions[1], hours[1]);
    EXPECT_EQ(partitions[2], hours[2]);
}

// ============================================================================
// 5. HDBReader: list_partitions_in_range 테스트
// ============================================================================
TEST_F(HDBTest, ListPartitionsInRange) {
    const SymbolId symbol = 5;
    HDBWriter writer(temp_dir_, false);

    // 5개 파티션 (1시간 간격)
    for (int i = 0; i < 5; ++i) {
        const int64_t h = static_cast<int64_t>(i + 1) * 3600LL * 1'000'000'000LL;
        auto part = make_partition(symbol, h, 10);
        writer.flush_partition(*part);
    }

    HDBReader reader(temp_dir_);

    // 2~4시간 구간 파티션 조회
    const int64_t from = 2 * 3600LL * 1'000'000'000LL;
    const int64_t to   = 4 * 3600LL * 1'000'000'000LL;
    const auto parts = reader.list_partitions_in_range(symbol, from, to);

    EXPECT_EQ(parts.size(), 3u);
}

// ============================================================================
// 6. FlushManager: 라이프사이클 테스트
// ============================================================================
TEST_F(HDBTest, FlushManager_Lifecycle) {
    PartitionManager pm(4ULL * 1024 * 1024);
    HDBWriter writer(temp_dir_, false);

    FlushConfig cfg{
        .memory_threshold    = 0.8,
        .check_interval_ms   = 50,   // 빠른 테스트를 위해 50ms
        .enable_compression  = false,
        .reclaim_after_flush = false, // 테스트용: 메모리 회수 비활성화
        .auto_seal_age_hours = 0,     // 즉시 봉인
    };

    FlushManager fm(pm, writer, cfg);

    EXPECT_FALSE(fm.running());
    fm.start();
    EXPECT_TRUE(fm.running());

    // 파티션 생성 및 봉인
    const int64_t hour = 1LL;
    Partition& part = pm.get_or_create(1, hour);
    part.add_column("timestamp", ColumnType::TIMESTAMP_NS);
    part.add_column("price",     ColumnType::INT64);
    part.add_column("volume",    ColumnType::INT64);
    part.add_column("msg_type",  ColumnType::INT32);
    for (int i = 0; i < 100; ++i) {
        part.get_column("timestamp")->append<int64_t>(static_cast<int64_t>(i));
        part.get_column("price")->append<int64_t>(1000LL + i);
        part.get_column("volume")->append<int64_t>(10LL);
        part.get_column("msg_type")->append<int32_t>(1);
    }
    part.seal();

    // 수동 플러시
    const size_t flushed = fm.flush_now();
    EXPECT_EQ(flushed, 1u) << "1개 파티션이 플러시되어야 함";

    const auto stats = fm.stats();
    EXPECT_EQ(stats.partitions_flushed, 1u);
    EXPECT_GT(stats.total_bytes_written, 0u);
    EXPECT_GE(stats.manual_flushes, 1u);

    fm.stop();
    EXPECT_FALSE(fm.running());
}

// ============================================================================
// 7. 빈 파티션 플러시 테스트 (엣지 케이스)
// ============================================================================
TEST_F(HDBTest, FlushEmptyPartition_NoFiles) {
    HDBWriter writer(temp_dir_, false);

    auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
        .total_size = 1024 * 1024,
        .use_hugepages = false,
    });

    PartitionKey key{1, 3600LL * 1'000'000'000LL};
    Partition empty_part(key, std::move(arena));
    empty_part.seal();

    // 행이 없으면 0 반환
    const size_t bytes = writer.flush_partition(empty_part);
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(writer.partitions_flushed(), 0u);
}

// ============================================================================
// 8. 존재하지 않는 컬럼 읽기 (엣지 케이스)
// ============================================================================
TEST_F(HDBTest, ReadNonExistentColumn_ReturnsInvalid) {
    const SymbolId symbol = 100;
    const int64_t  hour   = 3600LL * 1'000'000'000LL;

    {
        HDBWriter writer(temp_dir_, false);
        auto part = make_partition(symbol, hour, 100);
        writer.flush_partition(*part);
    }

    HDBReader reader(temp_dir_);
    auto col = reader.read_column(symbol, hour, "nonexistent_column");
    EXPECT_FALSE(col.valid()) << "존재하지 않는 컬럼은 invalid여야 함";
}

// ============================================================================
// 9. Tiered 쿼리 통합 테스트: RDB + HDB 혼합
// ============================================================================
TEST_F(HDBTest, TieredQuery_RdbAndHdb) {
    // HDB에 과거 파티션 미리 저장
    const SymbolId symbol   = 55;
    const int64_t  ns_hour  = 3600LL * 1'000'000'000LL;
    const int64_t  old_hour = 1 * ns_hour;
    const size_t   hdb_rows = 500;

    {
        HDBWriter writer(temp_dir_, false);
        auto part = make_partition(symbol, old_hour, hdb_rows,
                                   10000'0000LL, 100LL);
        writer.flush_partition(*part);
    }

    // Tiered 모드 파이프라인 구성
    PipelineConfig cfg{
        .arena_size_per_partition = 4ULL * 1024 * 1024,
        .storage_mode  = StorageMode::TIERED,
        .hdb_base_path = temp_dir_,
        .flush_config  = FlushConfig{.enable_compression = false},
    };
    auto pipeline = std::make_unique<ZeptoPipeline>(cfg);

    // 현재 시간 파티션에 RDB 데이터 삽입
    const int64_t  rdb_hour  = 3 * ns_hour;
    const size_t   rdb_rows  = 300;

    for (size_t i = 0; i < rdb_rows; ++i) {
        TickMessage msg{};
        msg.symbol_id = symbol;
        msg.recv_ts   = rdb_hour + static_cast<int64_t>(i) * 1'000'000LL;
        msg.price     = 20000'0000LL + static_cast<int64_t>(i);
        msg.volume    = 200LL;
        msg.msg_type  = 0;  // TRADE
        pipeline->ingest_tick(msg);
    }
    pipeline->drain_sync();

    // HDB에서만 조회 (old_hour 파티션)
    const auto hdb_result = pipeline->query_count(
        symbol, old_hour, old_hour + ns_hour - 1
    );
    EXPECT_EQ(hdb_result.ivalue, static_cast<int64_t>(hdb_rows))
        << "HDB 파티션 카운트 불일치";

    // RDB에서만 조회 — recv_ts는 TickPlant에서 현재 시각으로 덮어씌워짐
    // 따라서 full_scan으로 조회 (from=0, to=INT64_MAX)
    const auto rdb_result = pipeline->query_count(symbol, 0, INT64_MAX);
    // HDB + RDB 합산 결과 확인
    EXPECT_GE(rdb_result.ivalue, static_cast<int64_t>(rdb_rows))
        << "RDB + HDB 전체 카운트가 rdb_rows보다 작음";
    EXPECT_GE(rdb_result.ivalue, static_cast<int64_t>(hdb_rows))
        << "RDB + HDB 전체 카운트가 hdb_rows보다 작음";
}

// ============================================================================
// 10. MappedColumn RAII 이동 시맨틱 테스트
// ============================================================================
TEST_F(HDBTest, MappedColumn_MoveSemantics) {
    const SymbolId symbol = 77;
    const int64_t  hour   = 3600LL * 1'000'000'000LL;

    {
        HDBWriter writer(temp_dir_, false);
        auto part = make_partition(symbol, hour, 200);
        writer.flush_partition(*part);
    }

    HDBReader reader(temp_dir_);

    // 이동 생성자 테스트
    MappedColumn col1 = reader.read_column(symbol, hour, "price");
    ASSERT_TRUE(col1.valid());

    MappedColumn col2 = std::move(col1);
    EXPECT_FALSE(col1.valid()) << "이동 후 원본은 invalid여야 함";
    EXPECT_TRUE(col2.valid())  << "이동 대상은 valid여야 함";
    EXPECT_EQ(col2.num_rows, 200u);
}

// ============================================================================
// Data Durability: AutoSnapshot + Recovery
// ============================================================================

TEST_F(HDBTest, AutoSnapshot_CreatesFiles) {
    const fs::path snap_dir = fs::path(temp_dir_) / "snap";

    // Build a pipeline, ingest ticks, then snapshot via FlushManager
    PipelineConfig cfg;
    cfg.storage_mode     = StorageMode::TIERED;
    cfg.hdb_base_path    = (fs::path(temp_dir_) / "hdb").string();
    cfg.flush_config.enable_auto_snapshot  = true;
    cfg.flush_config.snapshot_interval_ms  = 60'000;  // won't fire on its own
    cfg.flush_config.snapshot_path         = snap_dir.string();
    cfg.flush_config.auto_seal_age_hours   = 999;     // keep partitions ACTIVE

    auto pipeline = std::make_unique<ZeptoPipeline>(cfg);

    const int64_t base_ts = 1'700'000'000LL * 1'000'000'000LL;
    for (int i = 0; i < 100; ++i) {
        TickMessage msg{};
        msg.symbol_id = 1;
        msg.recv_ts   = base_ts + i * 1'000'000LL;
        msg.price     = 100'0000LL + i;
        msg.volume    = 1000LL + i;
        pipeline->drain_sync(0);  // ensure store_tick path
        pipeline->store_tick_direct(msg);
    }

    ASSERT_EQ(pipeline->total_stored_rows(), 100u);

    // Trigger snapshot manually — must include ACTIVE partition
    ASSERT_NE(pipeline->flush_manager(), nullptr);
    const size_t snapped = pipeline->flush_manager()->snapshot_now();
    EXPECT_GE(snapped, 1u) << "at least one partition should be snapshotted";

    // Verify binary column files exist in snapshot dir
    bool found_price_file = false;
    for (auto& e : fs::recursive_directory_iterator(snap_dir)) {
        if (e.path().filename() == "price.bin") {
            found_price_file = true;
            break;
        }
    }
    EXPECT_TRUE(found_price_file) << "price.bin should exist under snapshot dir";
}

TEST_F(HDBTest, Recovery_ReloadsData) {
    const fs::path snap_dir = fs::path(temp_dir_) / "snap2";

    // --- Phase 1: ingest + snapshot ---
    {
        PipelineConfig cfg;
        cfg.storage_mode     = StorageMode::TIERED;
        cfg.hdb_base_path    = (fs::path(temp_dir_) / "hdb2").string();
        cfg.flush_config.snapshot_path       = snap_dir.string();
        cfg.flush_config.auto_seal_age_hours = 999;

        auto pipeline = std::make_unique<ZeptoPipeline>(cfg);

        const int64_t base_ts = 1'700'000'000LL * 1'000'000'000LL;
        const size_t N = 50;
        for (size_t i = 0; i < N; ++i) {
            TickMessage msg{};
            msg.symbol_id = 42;
            msg.recv_ts   = base_ts + static_cast<int64_t>(i) * 1'000'000LL;
            msg.price     = 200'0000LL + static_cast<int64_t>(i);
            msg.volume    = 500LL;
            pipeline->store_tick_direct(msg);
        }
        ASSERT_EQ(pipeline->total_stored_rows(), N);

        // Snapshot while still ACTIVE
        ASSERT_NE(pipeline->flush_manager(), nullptr);
        pipeline->flush_manager()->snapshot_now();
    }

    // --- Phase 2: new pipeline, recover from snapshot ---
    {
        PipelineConfig cfg;
        cfg.storage_mode              = StorageMode::PURE_IN_MEMORY;
        cfg.enable_recovery           = true;
        cfg.recovery_snapshot_path    = snap_dir.string();

        auto pipeline = std::make_unique<ZeptoPipeline>(cfg);
        pipeline->start();  // triggers recovery

        EXPECT_EQ(pipeline->total_stored_rows(), 50u)
            << "recovered pipeline should have all 50 rows";

        pipeline->stop();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

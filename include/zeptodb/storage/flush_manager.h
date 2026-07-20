#pragma once
// ============================================================================
// Layer 1: FlushManager — RDB → HDB 비동기 라이프사이클 관리자
// ============================================================================
// 설계 원칙:
//   - 백그라운드 스레드로 메모리 압력 모니터링
//   - 임계치(기본 80%) 초과 시 SEALED 파티션 HDB로 비동기 플러시
//   - 핫패스(인제스션) 완전 비차단 — SEALED 파티션만 처리
//   - Optional post-flush arena reclamation for HDB-aware consumers
//   - Lock-free 원칙: 핫패스에 mutex 없음
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/common/logger.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/parquet_writer.h"
#include "zeptodb/storage/s3_sink.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace zeptodb::storage {

// ============================================================================
// HDBOutputFormat: 저장 형식 선택
// ============================================================================
enum class HDBOutputFormat : uint8_t {
    BINARY,   ///< 기존 .bin 형식 (기본값, 최고속)
    PARQUET,  ///< Apache Parquet (Arrow 호환, DuckDB/Spark 상호운용)
    BOTH,     ///< BINARY + PARQUET 동시 저장
};

// ============================================================================
// FlushConfig: 플러시 매니저 설정
// ============================================================================
struct FlushConfig {
    /// 메모리 임계치 (0.0 ~ 1.0): 이 비율 초과 시 플러시 트리거
    double   memory_threshold    = 0.8;

    /// 메모리 모니터링 주기 (밀리초)
    uint32_t check_interval_ms   = 1000;

    /// LZ4 압축 사용 여부 (BINARY 형식)
    bool     enable_compression  = true;

    /// Whether to reclaim the arena after a successful flush. The default is
    /// false; ZeptoPipeline rejects true until general SQL merges HDB rows.
    bool     reclaim_after_flush = false;

    /// 파티션을 자동 봉인하는 나이 기준 (시간 단위)
    int64_t  auto_seal_age_hours = 1;

    // -----------------------------------------------------------------------
    // 출력 형식 옵션 (Parquet / S3)
    // -----------------------------------------------------------------------

    /// 저장 형식: BINARY (기본), PARQUET, BOTH
    HDBOutputFormat output_format = HDBOutputFormat::BINARY;

    /// Parquet 설정 (output_format == PARQUET or BOTH 시 적용)
    ParquetWriterConfig parquet_config;

    /// S3 업로드 활성화
    bool enable_s3_upload = false;

    /// S3 설정 (enable_s3_upload == true 시 적용)
    S3SinkConfig s3_config;

    /// S3 업로드 후 로컬 Parquet 파일 삭제 여부 (스토리지 절약)
    bool delete_local_after_s3 = false;

    // -----------------------------------------------------------------------
    // Snapshot options (graceful restart; not an abrupt-crash guarantee)
    // -----------------------------------------------------------------------

    /// Publish periodic RDB generations, including ACTIVE partitions.
    bool        enable_auto_snapshot = false;

    /// Snapshot interval in milliseconds (default: 60 seconds).
    uint32_t    snapshot_interval_ms = 60'000;

    /// Snapshot root containing CURRENT and immutable generations.
    std::string snapshot_path        = "";

    // -----------------------------------------------------------------------
    // Storage Tiering Policy (Hot → Warm → Cold → Drop)
    // -----------------------------------------------------------------------
    // All thresholds in nanoseconds. 0 = disabled (tier not used).
    //
    //   HOT  (in-memory)  → age < warm_after_ns
    //   WARM (local SSD)  → warm_after_ns ≤ age < cold_after_ns
    //   COLD (S3 Parquet) → cold_after_ns ≤ age < drop_after_ns
    //   DROP (deleted)    → age ≥ drop_after_ns
    //
    // Example: HOT 1h, WARM 24h, COLD 30d, DROP 365d
    //   warm_after_ns = 3'600'000'000'000
    //   cold_after_ns = 86'400'000'000'000
    //   drop_after_ns = 2'592'000'000'000'000 (30 days)
    //   (365d drop handled by TTL)

    struct TieringPolicy {
        bool    enabled       = false;
        int64_t warm_after_ns = 0;  // 0 = use auto_seal_age_hours instead
        int64_t cold_after_ns = 0;  // 0 = no cold tier
        int64_t drop_after_ns = 0;  // 0 = no auto-drop (use TTL)
    } tiering;
};

// ============================================================================
// FlushStats: 플러시 통계
// ============================================================================
struct FlushStats {
    uint64_t partitions_flushed  = 0;  // 총 플러시된 파티션 수
    uint64_t total_bytes_written = 0;  // 총 기록 바이트
    uint64_t flush_triggers      = 0;  // 임계치 초과로 인한 자동 플러시 횟수
    uint64_t manual_flushes      = 0;  // 수동 flush_now() 호출 횟수
    double   last_memory_ratio   = 0.0;// 마지막 메모리 사용률
    int64_t  last_flush_ns       = 0;  // 마지막 플러시 타임스탬프 (ns)
};

/// Result of resolving an atomically published snapshot generation.
/// `Missing` means no generation has been published yet; `Invalid` means a
/// snapshot root exists but its CURRENT/complete manifest is inconsistent.
enum class SnapshotResolutionStatus : uint8_t {
    Missing,
    Ready,
    Invalid,
};

struct SnapshotResolution {
    SnapshotResolutionStatus status = SnapshotResolutionStatus::Missing;
    std::string generation_path;
    size_t partition_count = 0;
    size_t row_count = 0;
    std::string error;
};

// ============================================================================
// FlushManager: 백그라운드 HDB 플러시 관리자
// ============================================================================
class FlushManager {
public:
    using EvictBeforeCallback = std::function<size_t(int64_t)>;

    /// @param pm      PartitionManager 참조 (소유권 없음)
    /// @param writer  HDBWriter 참조 (소유권 없음)
    /// @param config  플러시 설정
    /// @param evict_before Optional pipeline-owned eviction callback used to
    ///                     keep secondary partition indexes synchronized.
    FlushManager(PartitionManager& pm,
                 HDBWriter&         writer,
                 FlushConfig        config = {},
                 EvictBeforeCallback evict_before = {});

    ~FlushManager();

    // Non-copyable
    FlushManager(const FlushManager&) = delete;
    FlushManager& operator=(const FlushManager&) = delete;

    /// 백그라운드 플러시 스레드 시작
    void start();

    /// 백그라운드 플러시 스레드 중지 (join 대기)
    void stop();

    /// 수동 즉시 플러시 — 현재 모든 SEALED 파티션 동기 플러시
    /// @return 플러시된 파티션 수
    size_t flush_now();

    /// Publish one generation containing all current partitions, including
    /// ACTIVE partitions.
    /// @return Number of non-empty snapshotted partitions, or zero on failure
    ///         and for a valid empty generation. Check last_snapshot_succeeded().
    size_t snapshot_now();

    /// Whether the most recent snapshot attempt published a complete
    /// generation. Empty snapshots can succeed while snapshot_now() returns 0.
    [[nodiscard]] bool last_snapshot_succeeded() const {
        return last_snapshot_succeeded_.load(std::memory_order_acquire);
    }

    /// Resolve CURRENT under a snapshot root without accepting legacy partial
    /// directory layouts. Recovery must fail closed on `Invalid`.
    [[nodiscard]] static SnapshotResolution resolve_snapshot(
        const std::string& snapshot_path);

    /// Set TTL for automated partition eviction (0 = disabled).
    /// Thread-safe; takes effect on the next flush_loop() tick.
    void set_ttl(int64_t ttl_ns) {
        ttl_ns_.store(ttl_ns, std::memory_order_relaxed);
    }

    /// Set storage tiering policy at runtime (thread-safe).
    void set_tiering_policy(int64_t warm_ns, int64_t cold_ns, int64_t drop_ns) {
        config_.tiering.enabled       = (warm_ns > 0 || cold_ns > 0 || drop_ns > 0);
        config_.tiering.warm_after_ns = warm_ns;
        config_.tiering.cold_after_ns = cold_ns;
        config_.tiering.drop_after_ns = drop_ns;
    }

    [[nodiscard]] int64_t ttl_ns() const {
        return ttl_ns_.load(std::memory_order_relaxed);
    }

    /// 현재 통계 스냅샷
    [[nodiscard]] FlushStats stats() const;

    /// 현재 실행 중인지 여부
    [[nodiscard]] bool running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    /// 백그라운드 루프
    void flush_loop();

    /// 모든 SEALED 파티션 플러시 (내부 공통 로직)
    size_t do_flush_sealed();

    /// 모든 파티션(ACTIVE 포함) → snapshot_path 에 기록
    size_t do_snapshot();

    /// Storage tiering: age-based promotion from Hot → Warm → Cold → Drop
    void do_tiering();

    /// 단일 파티션 Parquet 저장 + 선택적 S3 업로드
    void flush_partition_parquet(const Partition& partition);

    /// 현재 시간 (나노초)
    static int64_t now_ns();

    PartitionManager&  pm_;
    HDBWriter&         writer_;
    FlushConfig        config_;
    EvictBeforeCallback evict_before_;

    // Parquet / S3 (선택적 — 설정에 따라 초기화)
    std::unique_ptr<ParquetWriter> parquet_writer_;
    std::unique_ptr<S3Sink>        s3_sink_;

    std::thread        flush_thread_;
    std::atomic<bool>  running_{false};

    // 통계 (원자적 업데이트)
    std::atomic<uint64_t> stat_partitions_flushed_{0};
    std::atomic<uint64_t> stat_bytes_written_{0};
    std::atomic<uint64_t> stat_flush_triggers_{0};
    std::atomic<uint64_t> stat_manual_flushes_{0};
    std::atomic<int64_t>  stat_last_flush_ns_{0};

    // 스냅샷 타이머
    std::atomic<int64_t>  last_snapshot_ns_{0};
    std::atomic<bool>     last_snapshot_succeeded_{false};
    std::mutex            snapshot_mu_;

    // TTL-based eviction (0 = disabled)
    std::atomic<int64_t>  ttl_ns_{0};

    // 마지막 메모리 비율 (double을 atomic으로 저장하기 위해 uint64 비트캐스트)
    std::atomic<uint64_t> stat_last_memory_ratio_bits_{0};
};

} // namespace zeptodb::storage

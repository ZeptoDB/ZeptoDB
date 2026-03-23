#pragma once
// ============================================================================
// APEX-DB: End-to-End Integration Pipeline
// ============================================================================
// 전체 파이프라인: TickPlant → PartitionManager → VectorizedEngine
//
// 설계 목표:
//   - 단일 API로 ingest → store → query 엔드투엔드 지원
//   - 백그라운드 드레인 스레드로 틱을 ColumnStore에 저장
//   - 쿼리는 저장된 컬럼 데이터에 직접 벡터화 실행
//   - StorageMode에 따라 HDB 계층도 함께 쿼리 (Tiered / Pure On-Disk)
// ============================================================================

#include "apex/common/types.h"
#include "apex/ingestion/tick_plant.h"
#include "apex/storage/partition_manager.h"
#include "apex/storage/hdb_writer.h"
#include "apex/storage/hdb_reader.h"
#include "apex/storage/flush_manager.h"
#include "apex/execution/vectorized_engine.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace apex::core {

using namespace apex::ingestion;
using namespace apex::storage;
using namespace apex::execution;

// ============================================================================
// StorageMode: N-4 스토리지 모드
// ============================================================================
enum class StorageMode : uint8_t {
    PURE_IN_MEMORY = 0,  // HFT 극단적 틱 처리 전용 (HDB 비활성화)
    TIERED         = 1,  // RDB(당일) + HDB(과거) 혼합 모드
    PURE_ON_DISK   = 2,  // 백테스트/딥러닝 전용 (HDB만 사용)
};

// ============================================================================
// QueryResult: 쿼리 결과 컨테이너
// ============================================================================
struct QueryResult {
    enum class Type : uint8_t {
        VWAP,
        SUM,
        COUNT,
        ERROR,
    };

    Type    type    = Type::ERROR;
    double  value   = 0.0;
    int64_t ivalue  = 0;       // 정수 결과 (SUM, COUNT)
    size_t  rows_scanned = 0;  // 스캔한 행 수
    int64_t latency_ns = 0;    // 쿼리 실행 시간 (ns)
    std::string error_msg;

    [[nodiscard]] bool ok() const { return type != Type::ERROR; }
};

// ============================================================================
// PipelineStats: 파이프라인 운영 통계
// ============================================================================
struct APEX_CACHE_ALIGNED PipelineStats {
    // 인제스션
    std::atomic<uint64_t> ticks_ingested{0};    // 총 수신 틱 수
    std::atomic<uint64_t> ticks_stored{0};      // 스토리지에 저장된 틱 수
    std::atomic<uint64_t> ticks_dropped{0};     // 드롭된 틱 (큐 오버플로우)

    // 쿼리
    std::atomic<uint64_t> queries_executed{0};  // 총 쿼리 실행 수
    std::atomic<uint64_t> total_rows_scanned{0};// 누적 스캔 행 수

    // 파티션
    std::atomic<uint64_t> partitions_created{0};

    // 지연
    std::atomic<int64_t>  last_ingest_latency_ns{0};

    // Non-copyable (atomic 멤버 때문에)
    PipelineStats() = default;
    PipelineStats(const PipelineStats&) = delete;
    PipelineStats& operator=(const PipelineStats&) = delete;
};

// ============================================================================
// PipelineConfig: 파이프라인 설정
// ============================================================================
struct PipelineConfig {
    // 파티션 아레나 크기 (기본 32MB)
    size_t arena_size_per_partition = 32ULL * 1024 * 1024;

    // 드레인 스레드 배치 크기
    size_t drain_batch_size = 256;

    // 드레인 스레드 sleep (마이크로초)
    uint32_t drain_sleep_us = 10;

    // 드레인 스레드 수 (1 = 기존 단일 스레드, >1 = 멀티 드레인)
    size_t drain_threads = 1;

    // -------------------------
    // HDB / Tiered Storage 설정
    // -------------------------

    /// N-4 스토리지 모드
    StorageMode storage_mode = StorageMode::PURE_IN_MEMORY;

    /// HDB 루트 디렉토리 (Tiered / Pure On-Disk 모드에서 사용)
    std::string hdb_base_path = "/tmp/apex_hdb";

    /// FlushManager 설정 (Tiered 모드)
    FlushConfig flush_config{};
};

// ============================================================================
// ApexPipeline: 엔드투엔드 파이프라인 메인 클래스
// ============================================================================
class ApexPipeline {
public:
    explicit ApexPipeline(PipelineConfig config = {});
    ~ApexPipeline();

    // Non-copyable
    ApexPipeline(const ApexPipeline&) = delete;
    ApexPipeline& operator=(const ApexPipeline&) = delete;

    /// 파이프라인 시작 (드레인 스레드 기동)
    void start();

    /// 파이프라인 중지 (드레인 스레드 종료, 큐 플러시)
    void stop();

    /// 틱 인제스트 (Thread-safe, lock-free)
    /// @return true if successfully queued
    bool ingest_tick(TickMessage msg);

    // ===== 쿼리 API =====

    /// VWAP 쿼리: symbol의 [from, to] 구간 VWAP 계산
    QueryResult query_vwap(SymbolId symbol,
                           Timestamp from = 0,
                           Timestamp to = INT64_MAX);

    /// Filter+Sum 쿼리: column > threshold인 rows의 sum(column) 반환
    QueryResult query_filter_sum(SymbolId symbol,
                                 const std::string& column,
                                 int64_t threshold,
                                 Timestamp from = 0,
                                 Timestamp to = INT64_MAX);

    /// Full scan: symbol의 총 행 수 반환
    QueryResult query_count(SymbolId symbol,
                            Timestamp from = 0,
                            Timestamp to = INT64_MAX);

    // ===== 통계 =====
    [[nodiscard]] const PipelineStats& stats() const { return stats_; }

    /// 현재 저장된 총 행 수 (모든 파티션 합산)
    [[nodiscard]] size_t total_stored_rows() const;

    /// 파티션 매니저 직접 접근 (테스트/벤치용)
    [[nodiscard]] PartitionManager& partition_manager() { return partition_mgr_; }
    [[nodiscard]] TickPlant& tick_plant() { return tick_plant_; }

    /// HDB 리더 접근 (Tiered/OnDisk 모드에서만 유효)
    [[nodiscard]] HDBReader* hdb_reader() { return hdb_reader_.get(); }

    /// FlushManager 접근 (Tiered 모드에서만 유효)
    [[nodiscard]] FlushManager* flush_manager() { return flush_manager_.get(); }

    /// 큐 강제 드레인 (테스트용: 백그라운드 스레드 없이 동기 드레인)
    size_t drain_sync(size_t max_items = SIZE_MAX);

private:
    // ============================================================
    // 내부 타입
    // ============================================================

    // 파티션 내 컬럼 스냅샷 (zero-copy 포인터)
    struct ColumnSnapshot {
        const int64_t* prices     = nullptr;
        const int64_t* volumes    = nullptr;
        const int64_t* timestamps = nullptr;
        const int64_t* extra_col  = nullptr;  // query_filter_sum용 추가 컬럼
        size_t         count      = 0;
    };

    // ============================================================
    // 내부 함수
    // ============================================================

    // 파티션에 틱 저장
    void store_tick(const TickMessage& msg);

    // 드레인 스레드 루프
    void drain_loop();

    // symbol 기준으로 저장된 파티션 목록 반환
    std::vector<Partition*> find_partitions(SymbolId symbol) const;

    // 파티션에서 ColumnSnapshot 빌드
    ColumnSnapshot build_snapshot(Partition* part, const std::string& extra_col_name) const;

    // HDB에서 시간 범위 내 COUNT 집계
    size_t hdb_count_range(SymbolId symbol, Timestamp from, Timestamp to) const;

    // ============================================================
    // 멤버
    // ============================================================

    PipelineConfig   config_;
    TickPlant        tick_plant_;
    PartitionManager partition_mgr_;

    PipelineStats    stats_;

    // symbol → partition 인덱스
    mutable std::mutex               partition_index_mu_;
    std::unordered_map<SymbolId, std::vector<Partition*>> partition_index_;

    // HDB 컴포넌트 (Tiered / Pure On-Disk 모드에서만 생성)
    std::unique_ptr<HDBWriter>    hdb_writer_;
    std::unique_ptr<HDBReader>    hdb_reader_;
    std::unique_ptr<FlushManager> flush_manager_;

    std::vector<std::thread> drain_threads_;
    std::atomic<bool> running_{false};
};

} // namespace apex::core


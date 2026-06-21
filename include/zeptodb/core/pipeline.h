#pragma once
// ============================================================================
// ZeptoDB: End-to-End Integration Pipeline
// ============================================================================
// 전체 파이프라인: TickPlant → PartitionManager → VectorizedEngine
//
// 설계 목표:
//   - 단일 API로 ingest → store → query 엔드투엔드 지원
//   - 백그라운드 드레인 스레드로 틱을 ColumnStore에 저장
//   - 쿼리는 저장된 컬럼 데이터에 직접 벡터화 실행
//   - StorageMode에 따라 HDB 계층도 함께 쿼리 (Tiered / Pure On-Disk)
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/storage/partition_manager.h"
#include "zeptodb/storage/schema_registry.h"
#include "zeptodb/storage/materialized_view.h"
#include "zeptodb/storage/hdb_writer.h"
#include "zeptodb/storage/hdb_reader.h"
#include "zeptodb/storage/flush_manager.h"
#include "zeptodb/execution/vectorized_engine.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeptodb::core {

using namespace zeptodb::ingestion;
using namespace zeptodb::storage;
using namespace zeptodb::execution;

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
struct ZEPTO_CACHE_ALIGNED PipelineStats {
    // 인제스션
    std::atomic<uint64_t> ticks_ingested{0};    // 총 수신 틱 수
    std::atomic<uint64_t> ticks_stored{0};      // 스토리지에 저장된 틱 수
    std::atomic<uint64_t> ticks_dropped{0};     // 드롭된 틱 (큐 오버플로우)

    // 쿼리
    std::atomic<uint64_t> queries_executed{0};  // 총 쿼리 실행 수
    std::atomic<uint64_t> total_rows_scanned{0};// 누적 스캔 행 수

    // 파티션
    std::atomic<uint64_t> partitions_created{0};
    std::atomic<uint64_t> partitions_evicted{0};

    // 지연
    std::atomic<int64_t>  last_ingest_latency_ns{0};

    // Non-copyable (atomic 멤버 때문에)
    PipelineStats() = default;
    PipelineStats(const PipelineStats&) = delete;
    PipelineStats& operator=(const PipelineStats&) = delete;
};

// ============================================================================
// TypedRowMessage: schema-aware wide-row ingest contract
// ============================================================================
// Used by connector paths that already decoded data into a table schema and
// should not force every value through TickMessage::price/volume.
// For SYMBOL/STRING values crossing RPC boundaries, has_string_value preserves
// the original text so the receiver can bind the same dictionary code.
struct TypedColumnValue {
    std::string name;
    ColumnType  type = ColumnType::INT64;
    int64_t     i64  = 0;
    double      f64  = 0.0;
    uint32_t    u32  = 0;
    uint8_t     u8   = 0;
    bool        has_string_value = false;
    std::string string_value;

    static TypedColumnValue int64(std::string name, int64_t value) {
        TypedColumnValue out;
        out.name = std::move(name);
        out.type = ColumnType::INT64;
        out.i64 = value;
        return out;
    }

    static TypedColumnValue timestamp(std::string name, int64_t value) {
        TypedColumnValue out;
        out.name = std::move(name);
        out.type = ColumnType::TIMESTAMP_NS;
        out.i64 = value;
        return out;
    }

    static TypedColumnValue float64(std::string name, double value) {
        TypedColumnValue out;
        out.name = std::move(name);
        out.type = ColumnType::FLOAT64;
        out.f64 = value;
        return out;
    }

    static TypedColumnValue symbol(std::string name, uint32_t value) {
        TypedColumnValue out;
        out.name = std::move(name);
        out.type = ColumnType::SYMBOL;
        out.u32 = value;
        return out;
    }

    static TypedColumnValue int32(std::string name, int32_t value) {
        TypedColumnValue out;
        out.name = std::move(name);
        out.type = ColumnType::INT32;
        out.i64 = value;
        return out;
    }
};

struct TypedRowMessage {
    uint16_t table_id = 0;
    SymbolId symbol_id = 0;
    Timestamp timestamp = 0;
    std::vector<TypedColumnValue> columns;
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

    /// Number of background drain threads that move ticks from the ring
    /// buffer into storage. The `MPMCRingBuffer` is lock-free MPMC so this
    /// scales ~linearly until PartitionManager lock contention dominates.
    ///
    /// Sentinel `0` = auto: at `start()` the pipeline uses
    /// `max(2, hardware_concurrency() / 4)`. Any explicit value `>=1` is
    /// honored exactly (the `std::max(1, ...)` clamp in `start()` keeps
    /// defense-in-depth for historical callers that wrote `0`). Raising
    /// this is the first knob when `ingest_tick()` falls back to the
    /// synchronous `store_tick()` path (a ~34× throughput cliff, devlog
    /// 102).
    size_t drain_threads = 0;

    /// Capacity (in slots) of the TickPlant MPMC ring buffer. Must be a
    /// power of two in `[4096, 16777216]`; `0` means "use engine default"
    /// (`kDefaultRingBufferCapacity` = 65536, backward-compat with pre-102
    /// builds). Raising this absorbs ingest bursts before the synchronous
    /// `store_tick()` fallback kicks in. The `ZeptoPipeline` constructor
    /// rejects invalid values (throws `std::invalid_argument`). See devlog 102.
    size_t ring_buffer_capacity = 0;

    // -------------------------
    // HDB / Tiered Storage 설정
    // -------------------------

    /// N-4 스토리지 모드
    StorageMode storage_mode = StorageMode::PURE_IN_MEMORY;

    /// HDB 루트 디렉토리 (Tiered / Pure On-Disk 모드에서 사용)
    std::string hdb_base_path = "/tmp/zepto_hdb";

    /// FlushManager 설정 (Tiered 모드)
    FlushConfig flush_config{};

    // -------------------------
    // Recovery 설정
    // -------------------------

    /// On start(), reload in-memory data from this snapshot directory.
    /// Works for all storage modes — points to the same path used by
    /// FlushConfig::snapshot_path (or a cold snapshot taken before shutdown).
    bool        enable_recovery          = false;
    std::string recovery_snapshot_path  = "";

    // -------------------------
    // Memory limit & eviction (P0-6)
    // -------------------------

    /// Maximum total memory for all partitions (0 = unlimited).
    /// When exceeded, oldest partitions are evicted (flushed to HDB if tiered).
    size_t max_memory_bytes = 0;
};

// ============================================================================
// ZeptoPipeline: 엔드투엔드 파이프라인 메인 클래스
// ============================================================================
class ZeptoPipeline {
public:
    explicit ZeptoPipeline(PipelineConfig config = {});
    ~ZeptoPipeline();

    // Non-copyable
    ZeptoPipeline(const ZeptoPipeline&) = delete;
    ZeptoPipeline& operator=(const ZeptoPipeline&) = delete;

    /// 파이프라인 시작 (드레인 스레드 기동)
    void start();

    /// 파이프라인 중지 (드레인 스레드 종료, 큐 플러시)
    void stop();

    /// Number of drain threads currently running (0 when stopped).
    [[nodiscard]] size_t drain_thread_count() const { return drain_threads_.size(); }

    /// Resolve a PipelineConfig::ring_buffer_capacity value to the effective
    /// runtime capacity. `0` → default (65536). Non-power-of-two, or outside
    /// `[4096, 16777216]`, throws `std::invalid_argument`. Used by the
    /// ZeptoPipeline ctor to size `TickPlant` and by operators / tests to
    /// pre-validate a config.
    static size_t resolve_ring_buffer_capacity(size_t configured);

    /// 틱 인제스트 (Thread-safe, lock-free)
    /// @return true if successfully queued
    bool ingest_tick(TickMessage msg);

    /// Schema-aware typed row ingest. This stores one wide row synchronously
    /// into the table/symbol/time partition identified by `row`, preserving
    /// native column types instead of mapping into TickMessage.
    /// @return false on invalid row, column type mismatch, or allocation error.
    bool ingest_typed_row(TypedRowMessage row);

    /// 배치 인제스트: 동일 symbol의 여러 틱을 한번에 큐잉
    /// @return 성공적으로 큐잉된 틱 수
    size_t ingest_batch(int32_t symbol,
                        const int64_t* prices,
                        const int64_t* volumes,
                        const int64_t* timestamps,  // nullptr이면 자동 생성
                        size_t count);

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

    /// SchemaRegistry 접근 (DDL 실행 후 스키마 조회)
    [[nodiscard]] SchemaRegistry& schema_registry() { return schema_registry_; }
    [[nodiscard]] const SchemaRegistry& schema_registry() const { return schema_registry_; }

    /// HDB base path (for schema catalog durability / table-scoped paths).
    /// Empty string if HDB is disabled (PURE_IN_MEMORY).
    [[nodiscard]] const std::string& hdb_base_path() const { return config_.hdb_base_path; }

    /// Persist SchemaRegistry to {hdb_base_path}/_schema.json. No-op if HDB
    /// is disabled. Called after CREATE / DROP / ALTER TABLE in executor.
    bool save_schema_catalog() const {
        if (config_.hdb_base_path.empty()) return false;
        return schema_registry_.save_to(config_.hdb_base_path + "/_schema.json");
    }

    /// MaterializedViewManager 접근
    [[nodiscard]] MaterializedViewManager& mat_view_manager() { return mat_view_mgr_; }

    /// Global symbol dictionary (string symbol name ↔ int32 symbol_id)
    [[nodiscard]] storage::StringDictionary& symbol_dict() { return symbol_dict_; }

    /// Evict all partitions whose hour_epoch is older than cutoff_ns,
    /// then rebuild partition_index_ to remove stale raw pointers.
    /// Used by ALTER TABLE SET TTL.
    /// @return number of partitions removed
    size_t evict_older_than_ns(int64_t cutoff_ns);

    /// Remove all partition_index_ entries belonging to the given table_id.
    /// Used by DROP TABLE after PartitionManager::drop_table_partitions.
    void drop_table_index(uint16_t table_id);

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

public:
    /// Ingest a tick directly into storage, bypassing the ring buffer.
    /// Preserves msg.recv_ts (no timestamp overwrite).
    void store_tick_direct(const TickMessage& msg) { store_tick(msg); }

private:

    // 드레인 스레드 루프
    void drain_loop();

    // symbol 기준으로 저장된 파티션 목록 반환 (backward-compat overload = table_id 0)
    std::vector<Partition*> find_partitions(SymbolId symbol) const {
        return find_partitions(0, symbol);
    }
    // Table-scoped partition lookup.
    std::vector<Partition*> find_partitions(uint16_t table_id, SymbolId symbol) const;

    // 파티션에서 ColumnSnapshot 빌드
    ColumnSnapshot build_snapshot(Partition* part, const std::string& extra_col_name) const;

    // HDB에서 시간 범위 내 COUNT 집계
    size_t hdb_count_range(SymbolId symbol, Timestamp from, Timestamp to) const;

    // Cold HDB fallback for query_vwap; extracted to keep the hot RDB scan
    // loop small enough to avoid stack spills (devlog 097).
    void query_vwap_hdb_fallback(SymbolId symbol, Timestamp from, Timestamp to,
                                 __int128& pv_sum, int64_t& v_sum,
                                 size_t& total_rows) const;

    // ============================================================
    // 멤버
    // ============================================================

    PipelineConfig   config_;
    TickPlant        tick_plant_;
    PartitionManager partition_mgr_;

    PipelineStats    stats_;

    // (table_id, symbol) → partitions
    mutable std::mutex               partition_index_mu_;
    std::unordered_map<uint64_t, std::vector<Partition*>> partition_index_;

    // Schema registry (all storage modes)
    SchemaRegistry schema_registry_;
    MaterializedViewManager mat_view_mgr_;
    storage::StringDictionary symbol_dict_;

    // HDB 컴포넌트 (Tiered / Pure On-Disk 모드에서만 생성)
    std::unique_ptr<HDBWriter>    hdb_writer_;
    std::unique_ptr<HDBReader>    hdb_reader_;
    std::unique_ptr<FlushManager> flush_manager_;

    std::vector<std::thread> drain_threads_;
    std::atomic<bool> running_{false};
};

} // namespace zeptodb::core

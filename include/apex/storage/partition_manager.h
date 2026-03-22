#pragma once
// ============================================================================
// Layer 1: Partition Manager — Symbol/Time 기반 파티셔닝 & HDB Flush
// ============================================================================
// 문서 근거: layer1_storage_memory.md §4 "파티셔닝(Partitioning)"
//   - Symbol별, 시간별(Hour) 단위로 파티션 청크 분리
//   - Read-Only 상태가 되면 HDB Flush 대상
// ============================================================================

#include "apex/common/types.h"
#include "apex/storage/arena_allocator.h"
#include "apex/storage/column_store.h"

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace apex::storage {

// ============================================================================
// PartitionKey: Symbol + Hour 단위 파티션 식별자
// ============================================================================
struct PartitionKey {
    SymbolId symbol_id;
    int64_t  hour_epoch;   // floor(timestamp / 3600e9) * 3600e9

    bool operator==(const PartitionKey& other) const {
        return symbol_id == other.symbol_id && hour_epoch == other.hour_epoch;
    }
};

struct PartitionKeyHash {
    size_t operator()(const PartitionKey& k) const {
        // FNV-1a style combine
        size_t h = static_cast<size_t>(k.symbol_id);
        h ^= static_cast<size_t>(k.hour_epoch) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// ============================================================================
// Partition: 하나의 파티션 청크 (RDB segment)
// ============================================================================
class Partition {
public:
    enum class State : uint8_t {
        ACTIVE,      // 쓰기 중
        SEALED,      // Read-Only (Flush 대기)
        FLUSHING,    // HDB로 내보내는 중
        FLUSHED,     // HDB 완료, 아레나 회수 가능
    };

    Partition(PartitionKey key, std::unique_ptr<ArenaAllocator> arena);

    [[nodiscard]] const PartitionKey& key() const { return key_; }
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] ArenaAllocator& arena() { return *arena_; }

    /// 컬럼 등록 (파티션 생성 시 스키마에 따라)
    ColumnVector& add_column(const std::string& name, ColumnType type);

    /// 컬럼 접근
    [[nodiscard]] ColumnVector* get_column(const std::string& name);
    [[nodiscard]] const ColumnVector* get_column(const std::string& name) const;
    [[nodiscard]] const std::vector<std::unique_ptr<ColumnVector>>& columns() const {
        return columns_;
    }

    /// 현재 행 수 (모든 컬럼 동일해야 함)
    [[nodiscard]] size_t num_rows() const;

    /// 파티션 봉인 (더 이상 쓰기 불가)
    void seal();

    /// 상태 전이
    void set_state(State s) { state_ = s; }

    /// 플러시 완료 후 아레나 메모리 회수 (재사용 불가, 데이터 무효화)
    void reclaim_arena() {
        if (arena_) {
            arena_->reset();
        }
    }

    // =========================================================================
    // 타임스탬프 범위 인덱스 (이진 탐색)
    // =========================================================================
    // 데이터는 append-only이므로 timestamp 컬럼은 항상 오름차순 정렬됨.
    // std::lower_bound / upper_bound를 사용해 O(log n) 범위 조회.
    //
    // 반환: [start_idx, end_idx) — 해당 범위에 속하는 행 인덱스 구간
    //
    std::pair<size_t, size_t> timestamp_range(int64_t from_ts, int64_t to_ts) const {
        const ColumnVector* ts_col = get_column("timestamp");
        if (!ts_col || ts_col->size() == 0) return {0, 0};
        // const_cast는 읽기 전용 접근이므로 안전
        auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
        // lower_bound: from_ts 이상인 첫 위치
        auto begin = std::lower_bound(span.begin(), span.end(), from_ts);
        // upper_bound: to_ts 초과하는 첫 위치
        auto end   = std::upper_bound(span.begin(), span.end(), to_ts);
        return {
            static_cast<size_t>(begin - span.begin()),
            static_cast<size_t>(end   - span.begin())
        };
    }

    /// 이 파티션의 타임스탬프 범위가 [lo, hi]와 겹치는지 O(1)로 확인
    /// (정렬 보장: 첫 행과 마지막 행만 비교하면 됨)
    bool overlaps_time_range(int64_t lo, int64_t hi) const {
        const ColumnVector* ts_col = get_column("timestamp");
        if (!ts_col || ts_col->size() == 0) return false;
        // const_cast는 읽기 전용 접근이므로 안전
        auto span = const_cast<ColumnVector*>(ts_col)->as_span<int64_t>();
        if (span.empty()) return false;
        // 파티션 전체가 [lo, hi] 밖에 있으면 false
        return span.front() <= hi && span.back() >= lo;
    }

private:
    PartitionKey                              key_;
    State                                     state_ = State::ACTIVE;
    std::unique_ptr<ArenaAllocator>           arena_;
    std::vector<std::unique_ptr<ColumnVector>> columns_;
};

// ============================================================================
// PartitionManager: 파티션 라우팅 & 라이프사이클 관리
// ============================================================================
class PartitionManager {
public:
    explicit PartitionManager(size_t arena_size_per_partition = DEFAULT_ARENA_SIZE);

    /// 틱 데이터의 Symbol + Timestamp로 적절한 파티션에 라우팅
    /// 없으면 자동 생성
    Partition& get_or_create(SymbolId symbol, Timestamp ts);

    /// 임계 메모리 도달 시 오래된 ACTIVE 파티션을 SEALED로 전환
    std::vector<Partition*> seal_old_partitions(Timestamp current_ts, int64_t max_age_hours = 1);

    /// SEALED 상태인 파티션 목록 반환 (FlushManager에서 사용)
    std::vector<Partition*> get_sealed_partitions();

    /// 전체 파티션 목록 반환 (SQL 쿼리 실행용)
    std::vector<Partition*> get_all_partitions();

    /// 특정 Symbol의 파티션 목록 반환
    std::vector<Partition*> get_partitions_for_symbol(SymbolId symbol);

    /// Return partitions whose hour_epoch window overlaps [lo, hi] (nanoseconds).
    /// Uses partition key comparison only — O(partitions), no data access.
    std::vector<Partition*> get_partitions_for_time_range(int64_t lo, int64_t hi);

    /// 전체 파티션 수
    [[nodiscard]] size_t partition_count() const { return partitions_.size(); }

private:
    static int64_t to_hour_epoch(Timestamp ts);

    size_t arena_size_;
    std::unordered_map<PartitionKey, std::unique_ptr<Partition>, PartitionKeyHash> partitions_;
    std::mutex mutex_;  // 파티션 생성 시에만 사용 (쓰기는 lock-free)
};

} // namespace apex::storage

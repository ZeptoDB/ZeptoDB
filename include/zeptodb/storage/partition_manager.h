#pragma once
// ============================================================================
// Layer 1: Partition Manager — Symbol/Time 기반 파티셔닝 & HDB Flush
// ============================================================================
// 문서 근거: layer1_storage_memory.md §4 "파티셔닝(Partitioning)"
//   - Symbol별, 시간별(Hour) 단위로 파티션 청크 분리
//   - Read-Only 상태가 되면 HDB Flush 대상
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/storage/arena_allocator.h"
#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace zeptodb::storage {

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
    // Attribute hints — s# (sorted column) binary-search index
    // =========================================================================
    // Mark a column as sorted (s# attribute). The column must already be
    // monotonically non-decreasing when this is called (append-only guarantee).
    void set_sorted(const std::string& col) { sorted_columns_.insert(col); }

    // Check if a column has the s# (sorted) attribute.
    bool is_sorted(const std::string& col) const {
        return sorted_columns_.count(col) > 0;
    }

    // O(log n) range scan on a sorted column.
    // Returns [begin_idx, end_idx) of rows where col value is in [lo, hi].
    std::pair<size_t, size_t> sorted_range(const std::string& col,
                                           int64_t lo, int64_t hi) const {
        const ColumnVector* cv = get_column(col);
        if (!cv || cv->size() == 0) return {0, 0};
        auto span = const_cast<ColumnVector*>(cv)->as_span<int64_t>();
        auto begin = std::lower_bound(span.begin(), span.end(), lo);
        auto end   = std::upper_bound(span.begin(), span.end(), hi);
        return {
            static_cast<size_t>(begin - span.begin()),
            static_cast<size_t>(end   - span.begin())
        };
    }

    // =========================================================================
    // g# (grouped/hash) index — O(1) equality lookup
    // =========================================================================
    // Builds a hash map from column value → list of row indices.
    // Ideal for high-cardinality columns with equality filters (WHERE col = X).

    void set_grouped(const std::string& col) {
        const ColumnVector* cv = get_column(col);
        if (!cv || cv->size() == 0) return;
        auto span = const_cast<ColumnVector*>(cv)->as_span<int64_t>();
        auto& idx = grouped_indexes_[col];
        idx.clear();
        idx.reserve(16);
        for (size_t i = 0; i < span.size(); ++i) {
            idx[span[i]].push_back(static_cast<uint32_t>(i));
        }
    }

    bool is_grouped(const std::string& col) const {
        return grouped_indexes_.count(col) > 0;
    }

    // O(1) lookup: returns row indices where col == value, or empty.
    const std::vector<uint32_t>& grouped_lookup(const std::string& col,
                                                 int64_t value) const {
        static const std::vector<uint32_t> empty;
        auto it = grouped_indexes_.find(col);
        if (it == grouped_indexes_.end()) return empty;
        auto vit = it->second.find(value);
        return (vit != it->second.end()) ? vit->second : empty;
    }

    // =========================================================================
    // p# (parted) index — O(1) range lookup for clustered data
    // =========================================================================
    // Records [first_row, last_row+1) for each distinct value.
    // Assumes data is clustered (same values are contiguous). Ideal for
    // low-cardinality columns like symbol, exchange, date.

    void set_parted(const std::string& col) {
        const ColumnVector* cv = get_column(col);
        if (!cv || cv->size() == 0) return;
        auto span = const_cast<ColumnVector*>(cv)->as_span<int64_t>();
        auto& idx = parted_indexes_[col];
        idx.clear();
        int64_t cur_val = span[0];
        size_t  cur_start = 0;
        for (size_t i = 1; i < span.size(); ++i) {
            if (span[i] != cur_val) {
                idx[cur_val] = {cur_start, i};
                cur_val = span[i];
                cur_start = i;
            }
        }
        idx[cur_val] = {cur_start, span.size()};
    }

    bool is_parted(const std::string& col) const {
        return parted_indexes_.count(col) > 0;
    }

    // O(1) lookup: returns [begin, end) row range for a parted value.
    std::pair<size_t, size_t> parted_range(const std::string& col,
                                            int64_t value) const {
        auto it = parted_indexes_.find(col);
        if (it == parted_indexes_.end()) return {0, 0};
        auto vit = it->second.find(value);
        return (vit != it->second.end()) ? vit->second : std::pair<size_t, size_t>{0, 0};
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
    std::unordered_set<std::string>           sorted_columns_;

    // g# index: col_name → { value → [row_indices] }
    std::unordered_map<std::string,
        std::unordered_map<int64_t, std::vector<uint32_t>>> grouped_indexes_;

    // p# index: col_name → { value → (begin, end) }
    std::unordered_map<std::string,
        std::unordered_map<int64_t, std::pair<size_t, size_t>>> parted_indexes_;
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

    /// Remove partitions whose hour_epoch is strictly before cutoff_ns.
    /// Used for TTL-based retention eviction.
    /// @return number of partitions removed
    size_t evict_older_than(int64_t cutoff_ns);

    /// Approximate total memory used by all partitions.
    size_t total_memory_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return partitions_.size() * arena_size_;
    }

    /// Return the partition with the smallest hour_epoch (oldest data).
    Partition* oldest_partition() {
        std::lock_guard<std::mutex> lock(mutex_);
        Partition* oldest = nullptr;
        int64_t min_hour = INT64_MAX;
        for (auto& [key, p] : partitions_) {
            if (key.hour_epoch < min_hour) {
                min_hour = key.hour_epoch;
                oldest = p.get();
            }
        }
        return oldest;
    }

    /// Evict (remove) a specific partition from the manager.
    void evict_partition(Partition* target) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = partitions_.begin(); it != partitions_.end(); ++it) {
            if (it->second.get() == target) {
                partitions_.erase(it);
                return;
            }
        }
    }

    /// 전체 파티션 수
    [[nodiscard]] size_t partition_count() const { return partitions_.size(); }

private:
    static int64_t to_hour_epoch(Timestamp ts);

    size_t arena_size_;
    std::unordered_map<PartitionKey, std::unique_ptr<Partition>, PartitionKeyHash> partitions_;
    mutable std::mutex mutex_;  // 파티션 생성 시에만 사용 (쓰기는 lock-free)
};

} // namespace zeptodb::storage

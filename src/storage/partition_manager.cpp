// ============================================================================
// Layer 1: Partition Manager Implementation
// ============================================================================

#include "zeptodb/storage/partition_manager.h"
#include <algorithm>

namespace zeptodb::storage {

// ============================================================================
// Partition
// ============================================================================
Partition::Partition(PartitionKey key, std::unique_ptr<ArenaAllocator> arena)
    : key_(key)
    , arena_(std::move(arena))
{
    ZEPTO_DEBUG("Partition created: table={}, symbol={}, hour={}",
               key_.table_id, key_.symbol_id, key_.hour_epoch);
}

ColumnVector& Partition::add_column(const std::string& name, ColumnType type) {
    auto col = std::make_unique<ColumnVector>(name, type, *arena_);
    auto& ref = *col;
    columns_.push_back(std::move(col));
    return ref;
}

ColumnVector* Partition::get_column(const std::string& name) {
    for (auto& col : columns_) {
        if (col->name() == name) return col.get();
    }
    return nullptr;
}

const ColumnVector* Partition::get_column(const std::string& name) const {
    for (const auto& col : columns_) {
        if (col->name() == name) return col.get();
    }
    return nullptr;
}

size_t Partition::num_rows() const {
    if (columns_.empty()) return 0;
    return columns_[0]->size();
}

void Partition::seal() {
    state_ = State::SEALED;
    ZEPTO_INFO("Partition sealed: symbol={}, hour={}, rows={}",
              key_.symbol_id, key_.hour_epoch, num_rows());
}

// ============================================================================
// PartitionManager
// ============================================================================
PartitionManager::PartitionManager(size_t arena_size_per_partition)
    : arena_size_(arena_size_per_partition)
{
}

int64_t PartitionManager::to_hour_epoch(Timestamp ts) {
    // ts is nanoseconds; floor to hour boundary
    constexpr int64_t NS_PER_HOUR = 3600LL * 1'000'000'000LL;
    return (ts / NS_PER_HOUR) * NS_PER_HOUR;
}

Partition& PartitionManager::get_or_create(uint16_t table_id, SymbolId symbol, Timestamp ts) {
    PartitionKey key{table_id, symbol, to_hour_epoch(ts)};

    // Fast path: check without lock (대부분의 경우 이미 존재)
    {
        auto it = partitions_.find(key);
        if (it != partitions_.end() && it->second->state() == Partition::State::ACTIVE) {
            return *it->second;
        }
    }

    // Slow path: 파티션 생성 (rare)
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = partitions_.find(key);
    if (it != partitions_.end()) {
        return *it->second;
    }

    auto arena = std::make_unique<ArenaAllocator>(ArenaConfig{
        .total_size = arena_size_,
        .use_hugepages = true,
        .numa_node = -1,
    });
    auto partition = std::make_unique<Partition>(key, std::move(arena));
    auto& ref = *partition;
    partitions_.emplace(key, std::move(partition));

    ZEPTO_INFO("New partition: table={}, symbol={}, hour={} (total partitions={})",
              table_id, symbol, key.hour_epoch, partitions_.size());
    return ref;
}

std::vector<Partition*> PartitionManager::seal_old_partitions(
    Timestamp current_ts, int64_t max_age_hours
) {
    constexpr int64_t NS_PER_HOUR = 3600LL * 1'000'000'000LL;
    int64_t cutoff = to_hour_epoch(current_ts) - (max_age_hours * NS_PER_HOUR);

    std::vector<Partition*> sealed;
    for (auto& [key, partition] : partitions_) {
        if (partition->state() == Partition::State::ACTIVE
            && key.hour_epoch < cutoff)
        {
            partition->seal();
            sealed.push_back(partition.get());
        }
    }
    return sealed;
}

std::vector<Partition*> PartitionManager::get_sealed_partitions() {
    std::vector<Partition*> result;
    for (auto& [key, partition] : partitions_) {
        if (partition->state() == Partition::State::SEALED) {
            result.push_back(partition.get());
        }
    }
    return result;
}

std::vector<Partition*> PartitionManager::get_all_partitions() {
    std::vector<Partition*> result;
    result.reserve(partitions_.size());
    for (auto& [key, partition] : partitions_) {
        result.push_back(partition.get());
    }
    return result;
}

std::vector<Partition*> PartitionManager::get_partitions_for_symbol(uint16_t table_id, SymbolId symbol) {
    std::vector<Partition*> result;
    for (auto& [key, partition] : partitions_) {
        if (key.table_id == table_id && key.symbol_id == symbol) {
            result.push_back(partition.get());
        }
    }
    return result;
}


std::vector<Partition*> PartitionManager::get_partitions_for_time_range(
    int64_t lo, int64_t hi)
{
    constexpr int64_t NS_PER_HOUR = 3600LL * 1'000'000'000LL;
    std::vector<Partition*> result;
    for (auto& [key, partition] : partitions_) {
        // Partition covers [hour_epoch, hour_epoch + NS_PER_HOUR).
        // Overlaps [lo, hi] iff hour_epoch <= hi && hour_epoch + NS_PER_HOUR > lo.
        if (key.hour_epoch <= hi && key.hour_epoch + NS_PER_HOUR > lo) {
            result.push_back(partition.get());
        }
    }
    return result;
}

std::vector<Partition*> PartitionManager::get_partitions_for_time_range(
    uint16_t table_id, int64_t lo, int64_t hi)
{
    constexpr int64_t NS_PER_HOUR = 3600LL * 1'000'000'000LL;
    std::vector<Partition*> result;
    for (auto& [key, partition] : partitions_) {
        if (key.table_id == table_id
            && key.hour_epoch <= hi
            && key.hour_epoch + NS_PER_HOUR > lo) {
            result.push_back(partition.get());
        }
    }
    return result;
}

std::vector<Partition*> PartitionManager::get_partitions_for_table(uint16_t table_id) {
    std::vector<Partition*> result;
    for (auto& [key, partition] : partitions_) {
        if (key.table_id == table_id) {
            result.push_back(partition.get());
        }
    }
    return result;
}

size_t PartitionManager::evict_older_than(int64_t cutoff_ns) {
    std::lock_guard<std::mutex> lk(mutex_);
    size_t count = 0;
    for (auto it = partitions_.begin(); it != partitions_.end(); ) {
        if (it->first.hour_epoch < cutoff_ns) {
            it = partitions_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

size_t PartitionManager::drop_table_partitions(uint16_t table_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    size_t count = 0;
    for (auto it = partitions_.begin(); it != partitions_.end(); ) {
        if (it->first.table_id == table_id) {
            it = partitions_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    return count;
}

} // namespace zeptodb::storage


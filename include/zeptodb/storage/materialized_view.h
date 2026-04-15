#pragma once
// ============================================================================
// ZeptoDB: Materialized View Manager
// ============================================================================
// Incremental aggregation on ingest — ClickHouse MaterializedView style.
// Each view defines a GROUP BY aggregation that is updated on every tick.
// Results are stored in a virtual table queryable via SELECT.
// ============================================================================

#include "zeptodb/common/types.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::storage {

// ============================================================================
// AggType: supported incremental aggregation functions
// ============================================================================
enum class MVAggType : uint8_t {
    SUM, COUNT, MIN, MAX, FIRST, LAST
};

// ============================================================================
// MVColumnDef: one output column of a materialized view
// ============================================================================
struct MVColumnDef {
    std::string name;       // output alias (e.g. "vol")
    MVAggType   agg;        // aggregation function
    std::string source_col; // source column (e.g. "volume", "price")
};

// ============================================================================
// MVDef: materialized view definition
// ============================================================================
struct MVDef {
    std::string              view_name;    // e.g. "ohlcv_5min"
    std::string              source_table; // e.g. "trades"
    std::vector<std::string> group_by;     // e.g. ["symbol"]
    int64_t                  xbar_bucket = 0; // time bucket ns (0 = no xbar)
    std::vector<MVColumnDef> columns;      // aggregation columns
};

// ============================================================================
// MVBucket: one aggregation bucket (one GROUP BY key combination)
// ============================================================================
struct MVBucket {
    // Keyed by group values (e.g. symbol_id, xbar_bucket_value)
    int64_t group_key  = 0;  // symbol or combined key
    int64_t time_key   = 0;  // xbar bucket value

    // Aggregation state per output column (indexed same as MVDef::columns)
    std::vector<int64_t> values;  // current aggregated values
    std::vector<int64_t> counts;  // for COUNT
};

// ============================================================================
// MaterializedViewManager
// ============================================================================
class MaterializedViewManager {
public:
    /// Register a new materialized view. Returns false if name already exists.
    bool create_view(MVDef def);

    /// Drop a materialized view. Returns false if not found.
    bool drop_view(const std::string& name);

    /// Check if a view exists.
    bool exists(const std::string& name) const;

    /// Called on every tick ingest — updates all registered views.
    /// @param symbol  tick symbol_id
    /// @param ts      tick timestamp (nanoseconds)
    /// @param price   tick price
    /// @param volume  tick volume
    void on_tick(int32_t symbol, int64_t ts, int64_t price, int64_t volume);

    /// Get view results as rows: [group_key, time_key, col0, col1, ...]
    /// Returns empty if view not found.
    struct ViewResult {
        std::vector<std::string>          column_names;
        std::vector<std::vector<int64_t>> rows;
    };
    ViewResult query(const std::string& view_name) const;

    /// List all registered view names.
    std::vector<std::string> list_views() const;

    /// Get all registered view definitions (for query rewrite matching).
    std::vector<MVDef> get_all_defs() const;

private:
    struct ViewState {
        MVDef def;
        // Buckets keyed by (group_key, time_key)
        using BucketKey = std::pair<int64_t, int64_t>;
        struct BucketKeyHash {
            size_t operator()(const BucketKey& k) const {
                return std::hash<int64_t>()(k.first) ^ (std::hash<int64_t>()(k.second) << 32);
            }
        };
        std::unordered_map<BucketKey, MVBucket, BucketKeyHash> buckets;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, ViewState> views_;

    // Update a single bucket with a new tick
    static void update_bucket(MVBucket& bucket, const MVDef& def,
                              int64_t price, int64_t volume);
};

} // namespace zeptodb::storage

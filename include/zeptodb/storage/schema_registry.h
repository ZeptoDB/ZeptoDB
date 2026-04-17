#pragma once
// ============================================================================
// Layer 1: SchemaRegistry — User-defined table schema store
// ============================================================================
// Design principles:
//   - Header-only; no .cpp dependency
//   - Thread-safe: shared_mutex (multi-reader / single-writer)
//   - One registry per ZeptoPipeline (not a global singleton)
//   - Stores column definitions (name + ColumnType) and TTL for each table
// ============================================================================

#include "zeptodb/storage/column_store.h"

#include <algorithm>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeptodb::storage {

// ============================================================================
// ColumnDef: a single column definition in a CREATE TABLE statement
// ============================================================================
struct ColumnDef {
    std::string name;
    ColumnType  type = ColumnType::INT64;
};

// ============================================================================
// TableSchema: schema for one named table
// ============================================================================
struct TableSchema {
    std::string            table_name;
    std::vector<ColumnDef> columns;
    int64_t                ttl_ns = 0;  // 0 = no retention limit
    bool                   has_data = false;  // set true on first INSERT
};

// ============================================================================
// SchemaRegistry: thread-safe table schema store
// ============================================================================
class SchemaRegistry {
public:
    // Create a new table. Returns false if the table already exists.
    bool create(const std::string& name, std::vector<ColumnDef> cols) {
        std::unique_lock lk(mu_);
        if (tables_.count(name)) return false;
        TableSchema s;
        s.table_name = name;
        s.columns    = std::move(cols);
        tables_[name] = std::move(s);
        return true;
    }

    // Drop a table. Returns false if the table did not exist.
    bool drop(const std::string& name) {
        std::unique_lock lk(mu_);
        return tables_.erase(name) > 0;
    }

    [[nodiscard]] bool exists(const std::string& name) const {
        std::shared_lock lk(mu_);
        return tables_.count(name) > 0;
    }

    // Returns a copy of the schema (safe across lock release).
    [[nodiscard]] std::optional<TableSchema> get(const std::string& name) const {
        std::shared_lock lk(mu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) return std::nullopt;
        return it->second;
    }

    bool add_column(const std::string& name, ColumnDef col) {
        std::unique_lock lk(mu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) return false;
        it->second.columns.push_back(std::move(col));
        return true;
    }

    bool drop_column(const std::string& name, const std::string& col_name) {
        std::unique_lock lk(mu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) return false;
        auto& cols = it->second.columns;
        auto ci = std::find_if(cols.begin(), cols.end(),
            [&](const ColumnDef& c) { return c.name == col_name; });
        if (ci == cols.end()) return false;
        cols.erase(ci);
        return true;
    }

    bool set_ttl(const std::string& name, int64_t ttl_ns) {
        std::unique_lock lk(mu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) return false;
        it->second.ttl_ns = ttl_ns;
        return true;
    }

    // Returns the minimum TTL in nanoseconds across all tables with TTL set.
    // Returns 0 if no table has a TTL configured.
    [[nodiscard]] int64_t min_ttl_ns() const {
        std::shared_lock lk(mu_);
        int64_t min = 0;
        for (auto& [_, s] : tables_) {
            if (s.ttl_ns > 0 && (min == 0 || s.ttl_ns < min)) min = s.ttl_ns;
        }
        return min;
    }

    [[nodiscard]] std::vector<std::string> list_tables() const {
        std::shared_lock lk(mu_);
        std::vector<std::string> names;
        names.reserve(tables_.size());
        for (auto& [k, _] : tables_) names.push_back(k);
        return names;
    }

    [[nodiscard]] size_t table_count() const {
        std::shared_lock lk(mu_);
        return tables_.size();
    }

    void mark_has_data(const std::string& name) {
        std::unique_lock lk(mu_);
        auto it = tables_.find(name);
        if (it != tables_.end()) it->second.has_data = true;
    }

    [[nodiscard]] bool has_data(const std::string& name) const {
        std::shared_lock lk(mu_);
        auto it = tables_.find(name);
        return it != tables_.end() && it->second.has_data;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, TableSchema> tables_;
};

} // namespace zeptodb::storage

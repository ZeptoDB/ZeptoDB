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
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace zeptodb::storage {

// ============================================================================
// ColumnDef: a single column definition in a CREATE TABLE statement
// ============================================================================
struct ColumnDef {
    std::string name;
    ColumnType  type = ColumnType::INT64;
};

enum class TablePlacementMode : uint8_t {
    HashByTableAndSymbol = 0,
    HashByTable          = 1,
    PinnedNode           = 2,
};

struct TablePlacementOptions {
    bool configured = false;
    TablePlacementMode mode = TablePlacementMode::HashByTableAndSymbol;
    uint32_t node_id = 0;
};

// ============================================================================
// TableSchema: schema for one named table
// ============================================================================
struct TableSchema {
    std::string            table_name;
    std::vector<ColumnDef> columns;
    int64_t                ttl_ns = 0;  // 0 = no retention limit
    bool                   has_data = false;  // set true on first INSERT
    uint16_t               table_id = 0;  // stable name-derived id (0 = legacy/default)
    TablePlacementOptions  placement;
};

enum class SchemaCatalogLoadResult : uint8_t {
    Loaded,
    NotFound,
    Invalid,
};

// ============================================================================
// SchemaRegistry: thread-safe table schema store
// ============================================================================
class SchemaRegistry {
public:
    // Stable cluster-wide table id derived from the table name. 0 remains the
    // legacy/default id, so the mapping is restricted to [1, 65535].
    [[nodiscard]] static uint16_t stable_table_id(const std::string& name) {
        uint32_t h = 2166136261u;  // FNV-1a 32-bit
        for (unsigned char c : name) {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        return static_cast<uint16_t>((h % 65535u) + 1u);
    }

    // Create a new table. Returns false if the table already exists.
    bool create(const std::string& name,
                std::vector<ColumnDef> cols,
                TablePlacementOptions placement = {}) {
        std::unique_lock lk(mu_);
        if (tables_.count(name)) return false;
        const uint16_t table_id = allocate_table_id_locked_(name);
        if (table_id == 0) {
            return false;
        }
        TableSchema s;
        s.table_name = name;
        s.columns    = std::move(cols);
        s.table_id   = table_id;
        s.placement  = placement;
        if (next_table_id_ <= table_id &&
            table_id < std::numeric_limits<uint16_t>::max()) {
            next_table_id_ = static_cast<uint16_t>(table_id + 1);
        }
        tables_[name] = std::move(s);
        return true;
    }

    // Drop a table. Returns false if the table did not exist.
    bool drop(const std::string& name) {
        std::unique_lock lk(mu_);
        return tables_.erase(name) > 0;
    }

    /// Restore an exact table schema snapshot after a catalog persistence
    /// failure. The original table id and all durable metadata are preserved.
    /// Refuse a snapshot that would collide with another table's id.
    bool restore_table_schema(TableSchema schema) {
        if (schema.table_name.empty() || schema.table_id == 0) return false;

        std::unique_lock lk(mu_);
        for (const auto& [name, existing] : tables_) {
            if (name != schema.table_name &&
                existing.table_id == schema.table_id) {
                return false;
            }
        }

        const std::string name = schema.table_name;
        const uint16_t table_id = schema.table_id;
        tables_[name] = std::move(schema);
        if (next_table_id_ <= table_id &&
            table_id < std::numeric_limits<uint16_t>::max()) {
            next_table_id_ = static_cast<uint16_t>(table_id + 1);
        }
        return true;
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

    // Returns a copy of the schema by stable table_id.
    [[nodiscard]] std::optional<TableSchema> get(uint16_t table_id) const {
        std::shared_lock lk(mu_);
        for (const auto& [_, schema] : tables_) {
            if (schema.table_id == table_id) return schema;
        }
        return std::nullopt;
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

    bool set_placement(const std::string& name,
                       TablePlacementOptions placement) {
        std::unique_lock lk(mu_);
        auto it = tables_.find(name);
        if (it == tables_.end()) return false;
        it->second.placement = placement;
        return true;
    }

    bool clear_placement(const std::string& name) {
        return set_placement(name, TablePlacementOptions{});
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

    void mark_has_data(uint16_t table_id) {
        if (table_id == 0) return;
        std::unique_lock lk(mu_);
        for (auto& [_, schema] : tables_) {
            if (schema.table_id == table_id) {
                schema.has_data = true;
                return;
            }
        }
    }

    [[nodiscard]] bool has_data(const std::string& name) const {
        std::shared_lock lk(mu_);
        auto it = tables_.find(name);
        return it != tables_.end() && it->second.has_data;
    }

    // Returns the stable table_id assigned at CREATE, or 0 if the table does not exist.
    // table_id = 0 is reserved for the legacy/default (no CREATE TABLE) path.
    [[nodiscard]] uint16_t get_table_id(const std::string& name) const {
        std::shared_lock lk(mu_);
        auto it = tables_.find(name);
        return it == tables_.end() ? uint16_t{0} : it->second.table_id;
    }

    // --------------------------------------------------------------------
    // JSON durability
    // --------------------------------------------------------------------
    // Minimal hand-rolled JSON: the catalog has only simple scalars (string
    // names, enum-int column types, int64 ttl, uint16 table_id), so we skip
    // a full JSON dep. Format:
    //   {"next_table_id":N,"tables":[{"name":"...","table_id":T,"ttl_ns":L,
    //     "has_data":0|1,"placement_configured":0|1,"placement_policy":N,
    //     "placement_node_id":N,"columns":[{"name":"c","type":N},...]},...]}

    bool save_to(const std::string& path) const {
        // devlog 088: DDL hot-path optimization.
        // 1) Serialize save calls so an older snapshot can never rename after
        //    a newer one and erase a concurrently completed DDL update.
        // 2) Snapshot the catalog under a shared_lock (readers don't block).
        // 3) Release the registry lock and write JSON to a per-(pid, tid) tmp
        //    file. Mutations may proceed while the slow I/O is in progress;
        //    their subsequent save observes the cumulative registry state.
        // 4) Atomically replace the final catalog file.
        std::lock_guard save_lock(save_mu_);
        uint16_t snap_next_id = 0;
        std::vector<TableSchema> snap;
        {
            std::shared_lock lk(mu_);
            snap_next_id = next_table_id_;
            snap.reserve(tables_.size());
            for (auto& [_, s] : tables_) snap.push_back(s);
        }
#if defined(__linux__)
        const long tid = ::syscall(SYS_gettid);
#else
        const long tid = static_cast<long>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
        std::string tmp = path + ".tmp."
            + std::to_string(static_cast<long>(::getpid()))
            + "." + std::to_string(tid);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "{\"next_table_id\":" << snap_next_id << ",\"tables\":[";
        bool first = true;
        for (auto& s : snap) {
            if (!first) out << ",";
            first = false;
            out << "{\"name\":" << json_str_(s.table_name)
                << ",\"table_id\":" << s.table_id
                << ",\"ttl_ns\":"   << s.ttl_ns
                << ",\"has_data\":" << (s.has_data ? 1 : 0)
                << ",\"placement_configured\":"
                << (s.placement.configured ? 1 : 0)
                << ",\"placement_policy\":"
                << static_cast<int>(s.placement.mode)
                << ",\"placement_node_id\":"
                << s.placement.node_id
                << ",\"columns\":[";
            for (size_t i = 0; i < s.columns.size(); ++i) {
                if (i) out << ",";
                out << "{\"name\":" << json_str_(s.columns[i].name)
                    << ",\"type\":" << static_cast<int>(s.columns[i].type)
                    << "}";
            }
            out << "]}";
        }
        out << "]}";
        out.close();
        if (!out) {
            std::remove(tmp.c_str());
            return false;
        }
#if defined(__linux__)
        // Persist the new file before publishing its name.  Atomic rename
        // alone does not make the file durable across power loss.
        const int tmp_fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
        if (tmp_fd < 0) {
            std::remove(tmp.c_str());
            return false;
        }
        const bool file_synced = ::fsync(tmp_fd) == 0;
        ::close(tmp_fd);
        if (!file_synced) {
            std::remove(tmp.c_str());
            return false;
        }
#endif
        // save_mu_ serializes the final path; mu_ keeps a concurrent registry
        // mutation out of the rename window.
        std::unique_lock lk(mu_);
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            return false;
        }
#if defined(__linux__)
        const auto parent = std::filesystem::path(path).parent_path();
        const std::string parent_path = parent.empty() ? "." : parent.string();
        const int dir_fd = ::open(
            parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0) return false;
        const bool directory_synced = ::fsync(dir_fd) == 0;
        ::close(dir_fd);
        if (!directory_synced) return false;
#endif
        return true;
    }

    [[nodiscard]] SchemaCatalogLoadResult load_from_checked(
        const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            return !exists && !error
                ? SchemaCatalogLoadResult::NotFound
                : SchemaCatalogLoadResult::Invalid;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        if (!in.good() && !in.eof()) return SchemaCatalogLoadResult::Invalid;
        const std::string s = ss.str();

        auto skip_ws = [](const std::string& value, size_t& pos, size_t end) {
            while (pos < end &&
                   (value[pos] == ' ' || value[pos] == '\n' ||
                    value[pos] == '\r' || value[pos] == '\t')) {
                ++pos;
            }
        };
        size_t root_start = 0;
        skip_ws(s, root_start, s.size());
        if (root_start >= s.size() || s[root_start] != '{') {
            return SchemaCatalogLoadResult::Invalid;
        }
        const size_t root_end = json_match_brace_(s, root_start);
        if (root_end == std::string::npos) {
            return SchemaCatalogLoadResult::Invalid;
        }
        size_t trailing = root_end + 1;
        skip_ws(s, trailing, s.size());
        if (trailing != s.size()) return SchemaCatalogLoadResult::Invalid;

        uint32_t loaded_next = 0;
        if (json_find_int_(s, "next_table_id", root_start, loaded_next) == 0 ||
            loaded_next == 0 ||
            loaded_next > std::numeric_limits<uint16_t>::max()) {
            return SchemaCatalogLoadResult::Invalid;
        }
        const size_t tables_value =
            json_find_member_value_(s, "tables", root_start);
        if (tables_value == std::string::npos || tables_value > root_end) {
            return SchemaCatalogLoadResult::Invalid;
        }
        const size_t lbracket = tables_value;
        const size_t rbracket = json_match_bracket_(s, lbracket);
        if (lbracket == std::string::npos || rbracket == std::string::npos ||
            rbracket > root_end) {
            return SchemaCatalogLoadResult::Invalid;
        }
        size_t after_tables = rbracket + 1;
        skip_ws(s, after_tables, root_end);
        if (after_tables != root_end) {
            return SchemaCatalogLoadResult::Invalid;
        }

        std::unordered_map<std::string, TableSchema> parsed_tables;
        std::unordered_set<uint16_t> parsed_ids;
        uint16_t max_id = 0;
        size_t i = lbracket + 1;
        while (i < rbracket) {
            skip_ws(s, i, rbracket);
            if (i == rbracket) break;
            if (s[i] != '{') return SchemaCatalogLoadResult::Invalid;
            const size_t obj_end = json_match_brace_(s, i);
            if (obj_end == std::string::npos || obj_end > rbracket) {
                return SchemaCatalogLoadResult::Invalid;
            }
            const std::string obj = s.substr(i, obj_end - i + 1);
            const size_t columns_left =
                json_find_member_value_(obj, "columns", 0);
            const size_t columns_right =
                json_match_bracket_(obj, columns_left);
            if (columns_left == std::string::npos ||
                columns_right == std::string::npos) {
                return SchemaCatalogLoadResult::Invalid;
            }
            size_t after_columns = columns_right + 1;
            skip_ws(obj, after_columns, obj.size());
            if (after_columns + 1 != obj.size() || obj[after_columns] != '}') {
                return SchemaCatalogLoadResult::Invalid;
            }
            TableSchema table;
            uint32_t table_id = 0;
            int64_t ttl_ns = 0;
            uint32_t has_data = 0;
            if (!json_extract_string_(obj, "name", table.table_name) ||
                table.table_name.empty() ||
                json_find_int_(obj, "table_id", 0, table_id) == 0 ||
                table_id == 0 ||
                table_id > std::numeric_limits<uint16_t>::max() ||
                json_find_int64_(obj, "ttl_ns", 0, ttl_ns) == 0 ||
                ttl_ns < 0 ||
                json_find_int_(obj, "has_data", 0, has_data) == 0 ||
                has_data > 1) {
                return SchemaCatalogLoadResult::Invalid;
            }
            table.table_id = static_cast<uint16_t>(table_id);
            table.ttl_ns = ttl_ns;
            table.has_data = has_data != 0;
            if (parsed_tables.contains(table.table_name) ||
                !parsed_ids.insert(table.table_id).second) {
                return SchemaCatalogLoadResult::Invalid;
            }

            uint32_t placement_configured = 0;
            if (json_find_int_(obj, "placement_configured", 0,
                               placement_configured) != 0) {
                if (placement_configured > 1) {
                    return SchemaCatalogLoadResult::Invalid;
                }
                table.placement.configured = placement_configured != 0;
            }
            uint32_t placement_policy = 0;
            if (json_find_int_(obj, "placement_policy", 0,
                               placement_policy) != 0) {
                if (placement_policy >
                    static_cast<uint32_t>(TablePlacementMode::PinnedNode)) {
                    return SchemaCatalogLoadResult::Invalid;
                }
                table.placement.mode =
                    static_cast<TablePlacementMode>(placement_policy);
            }
            uint32_t placement_node_id = 0;
            if (json_find_int_(obj, "placement_node_id", 0,
                               placement_node_id) != 0) {
                table.placement.node_id = placement_node_id;
            }
            if (table.placement.configured &&
                table.placement.mode == TablePlacementMode::PinnedNode &&
                table.placement.node_id == 0) {
                return SchemaCatalogLoadResult::Invalid;
            }

            std::unordered_set<std::string> column_names;
            size_t column_pos = columns_left + 1;
            while (column_pos < columns_right) {
                skip_ws(obj, column_pos, columns_right);
                if (column_pos == columns_right) break;
                if (obj[column_pos] != '{') {
                    return SchemaCatalogLoadResult::Invalid;
                }
                const size_t column_end =
                    json_match_brace_(obj, column_pos);
                if (column_end == std::string::npos ||
                    column_end > columns_right) {
                    return SchemaCatalogLoadResult::Invalid;
                }
                const std::string column_obj =
                    obj.substr(column_pos, column_end - column_pos + 1);
                ColumnDef column;
                uint32_t column_type = 0;
                if (!json_extract_string_(column_obj, "name", column.name) ||
                    column.name.empty() ||
                    !column_names.insert(column.name).second ||
                    json_find_int_(column_obj, "type", 0, column_type) == 0 ||
                    column_type >
                        static_cast<uint32_t>(ColumnType::STRING)) {
                    return SchemaCatalogLoadResult::Invalid;
                }
                column.type = static_cast<ColumnType>(column_type);
                table.columns.push_back(std::move(column));
                column_pos = column_end + 1;
                skip_ws(obj, column_pos, columns_right);
                if (column_pos < columns_right) {
                    if (obj[column_pos] != ',') {
                        return SchemaCatalogLoadResult::Invalid;
                    }
                    ++column_pos;
                    skip_ws(obj, column_pos, columns_right);
                    if (column_pos == columns_right) {
                        return SchemaCatalogLoadResult::Invalid;
                    }
                }
            }
            if (table.table_id > max_id) max_id = table.table_id;
            parsed_tables.emplace(table.table_name, std::move(table));
            i = obj_end + 1;
            skip_ws(s, i, rbracket);
            if (i < rbracket) {
                if (s[i] != ',') return SchemaCatalogLoadResult::Invalid;
                ++i;
                skip_ws(s, i, rbracket);
                if (i == rbracket) return SchemaCatalogLoadResult::Invalid;
            }
        }

        uint32_t next_id = std::max<uint32_t>(
            loaded_next, static_cast<uint32_t>(max_id) + 1u);
        if (next_id > std::numeric_limits<uint16_t>::max()) {
            next_id = std::numeric_limits<uint16_t>::max();
        }
        {
            std::unique_lock lk(mu_);
            tables_.swap(parsed_tables);
            next_table_id_ = static_cast<uint16_t>(next_id);
        }
        return SchemaCatalogLoadResult::Loaded;
    }

    bool load_from(const std::string& path) {
        return load_from_checked(path) == SchemaCatalogLoadResult::Loaded;
    }

private:
    // ---- tiny JSON helpers (header-only) ----
    static std::string json_str_(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }
        out += '"';
        return out;
    }
    static size_t json_match_brace_(const std::string& s, size_t start) {
        if (start >= s.size() || s[start] != '{') return std::string::npos;
        int depth = 0;
        bool in_str = false;
        for (size_t i = start; i < s.size(); ++i) {
            char c = s[i];
            if (in_str) {
                if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
                if (c == '"') in_str = false;
            } else {
                if (c == '"') in_str = true;
                else if (c == '{') ++depth;
                else if (c == '}') { --depth; if (depth == 0) return i; }
            }
        }
        return std::string::npos;
    }
    static size_t json_match_bracket_(const std::string& s, size_t start) {
        if (start == std::string::npos || start >= s.size() || s[start] != '[') return std::string::npos;
        int depth = 0;
        bool in_str = false;
        for (size_t i = start; i < s.size(); ++i) {
            char c = s[i];
            if (in_str) {
                if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
                if (c == '"') in_str = false;
            } else {
                if (c == '"') in_str = true;
                else if (c == '[') ++depth;
                else if (c == ']') { --depth; if (depth == 0) return i; }
            }
        }
        return std::string::npos;
    }
    // Return the first byte of a top-level object member's value. Looking only
    // at depth one prevents a missing table field from being satisfied by a
    // same-named field inside a nested column object.
    static size_t json_find_member_value_(const std::string& s,
                                          const char* key,
                                          size_t start) {
        int object_depth = 0;
        int array_depth = 0;
        const std::string expected(key);
        for (size_t i = start; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '{') {
                ++object_depth;
                continue;
            }
            if (c == '}') {
                --object_depth;
                continue;
            }
            if (c == '[') {
                ++array_depth;
                continue;
            }
            if (c == ']') {
                --array_depth;
                continue;
            }
            if (c != '"') continue;

            const size_t text_start = i + 1;
            size_t text_end = text_start;
            for (; text_end < s.size(); ++text_end) {
                if (s[text_end] == '\\' && text_end + 1 < s.size()) {
                    ++text_end;
                    continue;
                }
                if (s[text_end] == '"') break;
            }
            if (text_end >= s.size()) return std::string::npos;
            if (object_depth == 1 && array_depth == 0 &&
                s.compare(text_start, text_end - text_start, expected) == 0) {
                size_t value = text_end + 1;
                while (value < s.size() &&
                       (s[value] == ' ' || s[value] == '\t' ||
                        s[value] == '\n' || s[value] == '\r')) {
                    ++value;
                }
                if (value < s.size() && s[value] == ':') {
                    ++value;
                    while (value < s.size() &&
                           (s[value] == ' ' || s[value] == '\t' ||
                            s[value] == '\n' || s[value] == '\r')) {
                        ++value;
                    }
                    return value;
                }
            }
            i = text_end;
        }
        return std::string::npos;
    }
    static bool json_extract_string_(const std::string& s, const char* key, std::string& out) {
        size_t p = json_find_member_value_(s, key, 0);
        if (p == std::string::npos) return false;
        if (p >= s.size() || s[p] != '"') return false;
        ++p;
        out.clear();
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                char n = s[p + 1];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '"': out += '"';  break;
                    case '\\': out += '\\'; break;
                    default: return false;
                }
                p += 2;
            } else {
                if (static_cast<unsigned char>(s[p]) < 0x20) return false;
                out += s[p++];
            }
        }
        if (p >= s.size()) return false;
        size_t after = p + 1;
        while (after < s.size() &&
               (s[after] == ' ' || s[after] == '\t' ||
                s[after] == '\n' || s[after] == '\r')) {
            ++after;
        }
        return after == s.size() || s[after] == ',' || s[after] == '}';
    }
    template <class T>
    static int json_find_int_gen_(const std::string& s, const char* key, size_t start, T& out) {
        static_assert(std::is_integral_v<T>);
        size_t p = json_find_member_value_(s, key, start);
        if (p == std::string::npos) return 0;
        bool neg = false;
        if (p < s.size() && s[p] == '-') { neg = true; ++p; }
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return 0;
        if (neg && std::is_unsigned_v<T>) return 0;
        const uint64_t positive_limit = static_cast<uint64_t>(
            std::numeric_limits<T>::max());
        const uint64_t magnitude_limit = neg
            ? positive_limit + 1u
            : positive_limit;
        uint64_t v = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            const uint64_t digit = static_cast<uint64_t>(s[p] - '0');
            if (v > (magnitude_limit - digit) / 10u) return 0;
            v = v * 10u + digit;
            ++p;
        }
        size_t after = p;
        while (after < s.size() &&
               (s[after] == ' ' || s[after] == '\t' ||
                s[after] == '\n' || s[after] == '\r')) {
            ++after;
        }
        if (after < s.size() && s[after] != ',' && s[after] != '}' &&
            s[after] != ']') {
            return 0;
        }
        if (neg) {
            if (v == positive_limit + 1u) {
                out = std::numeric_limits<T>::min();
            } else {
                out = static_cast<T>(-static_cast<int64_t>(v));
            }
        } else {
            out = static_cast<T>(v);
        }
        return 1;
    }
    static int json_find_int_(const std::string& s, const char* key, size_t start, uint32_t& out) {
        return json_find_int_gen_(s, key, start, out);
    }
    static int json_find_int64_(const std::string& s, const char* key, size_t start, int64_t& out) {
        return json_find_int_gen_(s, key, start, out);
    }

    [[nodiscard]] uint16_t allocate_table_id_locked_(const std::string& name) const {
        uint16_t candidate = stable_table_id(name);
        for (uint32_t attempt = 0; attempt < 65535u; ++attempt) {
            bool used = false;
            for (const auto& [_, schema] : tables_) {
                if (schema.table_id == candidate) {
                    used = true;
                    break;
                }
            }
            if (!used) return candidate;
            candidate = (candidate == std::numeric_limits<uint16_t>::max())
                ? uint16_t{1}
                : static_cast<uint16_t>(candidate + 1);
        }
        return 0;
    }

    mutable std::shared_mutex mu_;
    mutable std::mutex save_mu_;
    std::unordered_map<std::string, TableSchema> tables_;
    uint16_t next_table_id_ = 1;  // catalog compatibility; ids are name-derived on create
};

} // namespace zeptodb::storage

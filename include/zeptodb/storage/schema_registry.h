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
#include <cstdio>
#include <fstream>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
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

// ============================================================================
// TableSchema: schema for one named table
// ============================================================================
struct TableSchema {
    std::string            table_name;
    std::vector<ColumnDef> columns;
    int64_t                ttl_ns = 0;  // 0 = no retention limit
    bool                   has_data = false;  // set true on first INSERT
    uint16_t               table_id = 0;  // assigned by SchemaRegistry (0 = legacy/default)
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
        if (next_table_id_ == 0) {
            // uint16_t wrapped: >65535 CREATEs in this process lifetime.
            // Refuse to reuse table_id=0 (reserved for legacy path).
            return false;
        }
        TableSchema s;
        s.table_name = name;
        s.columns    = std::move(cols);
        s.table_id   = next_table_id_++;
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

    // Returns the table_id assigned at CREATE, or 0 if the table does not exist.
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
    //     "has_data":0|1,"columns":[{"name":"c","type":N},...]},...]}

    bool save_to(const std::string& path) const {
        // devlog 088: DDL hot-path optimization.
        // 1) Snapshot the catalog under a shared_lock (readers don't block).
        // 2) Release the lock and write JSON to a per-(pid, tid) tmp file
        //    with NO lock held — the slow I/O must not serialize DDL.
        // 3) Take a unique_lock only around std::rename so two concurrent
        //    save_to() calls on the same final path serialize that one
        //    syscall and we preserve atomic-replace semantics.
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
        if (!out) return false;
        // atomic replace (best-effort) — serialize final rename on same path
        std::unique_lock lk(mu_);
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }

    bool load_from(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string s = ss.str();
        std::unique_lock lk(mu_);
        tables_.clear();
        size_t p = 0;
        uint32_t loaded_next = 1;
        if (json_find_int_(s, "next_table_id", p, loaded_next) == 0) return false;
        // Find "tables":[ ... ]
        size_t tpos = s.find("\"tables\"", p);
        if (tpos == std::string::npos) return false;
        size_t lbracket = s.find('[', tpos);
        if (lbracket == std::string::npos) return false;
        size_t i = lbracket + 1;
        uint16_t max_id = 0;
        while (i < s.size()) {
            // skip whitespace/commas until '{' or ']'
            while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
            if (i >= s.size() || s[i] == ']') break;
            if (s[i] != '{') break;
            size_t obj_end = json_match_brace_(s, i);
            if (obj_end == std::string::npos) break;
            const std::string obj = s.substr(i, obj_end - i + 1);
            TableSchema ts;
            std::string tname;
            if (!json_extract_string_(obj, "name", tname)) { i = obj_end + 1; continue; }
            ts.table_name = tname;
            uint32_t tid = 0;
            json_find_int_(obj, "table_id", 0, tid);
            ts.table_id = static_cast<uint16_t>(tid);
            int64_t ttl_signed = 0;
            json_find_int64_(obj, "ttl_ns", 0, ttl_signed);
            ts.ttl_ns = ttl_signed;
            uint32_t hd_val = 0;
            json_find_int_(obj, "has_data", 0, hd_val);
            ts.has_data = (hd_val != 0);
            // columns
            size_t cpos = obj.find("\"columns\"");
            if (cpos != std::string::npos) {
                size_t cl = obj.find('[', cpos);
                size_t cr = json_match_bracket_(obj, cl);
                if (cl != std::string::npos && cr != std::string::npos) {
                    size_t j = cl + 1;
                    while (j < cr) {
                        while (j < cr && (obj[j] == ' ' || obj[j] == ',' || obj[j] == '\n' || obj[j] == '\r' || obj[j] == '\t')) ++j;
                        if (j >= cr || obj[j] != '{') break;
                        size_t cje = json_match_brace_(obj, j);
                        if (cje == std::string::npos || cje > cr) break;
                        std::string cobj = obj.substr(j, cje - j + 1);
                        ColumnDef cd;
                        json_extract_string_(cobj, "name", cd.name);
                        uint32_t ctype = 0;
                        json_find_int_(cobj, "type", 0, ctype);
                        cd.type = static_cast<ColumnType>(ctype);
                        ts.columns.push_back(std::move(cd));
                        j = cje + 1;
                    }
                }
            }
            if (ts.table_id > max_id) max_id = ts.table_id;
            tables_[ts.table_name] = std::move(ts);
            i = obj_end + 1;
        }
        // next_table_id must be > max loaded id; don't reset below loaded_next either.
        uint32_t nti = loaded_next;
        if (static_cast<uint32_t>(max_id) + 1u > nti) nti = static_cast<uint32_t>(max_id) + 1u;
        next_table_id_ = static_cast<uint16_t>(nti > 65535u ? 65535u : nti);
        return true;
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
    static bool json_extract_string_(const std::string& s, const char* key, std::string& out) {
        std::string needle = std::string("\"") + key + "\"";
        size_t p = s.find(needle);
        if (p == std::string::npos) return false;
        p = s.find(':', p);
        if (p == std::string::npos) return false;
        // skip ws
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
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
                    default: out += n; break;
                }
                p += 2;
            } else {
                out += s[p++];
            }
        }
        return p < s.size();
    }
    template <class T>
    static int json_find_int_gen_(const std::string& s, const char* key, size_t start, T& out) {
        std::string needle = std::string("\"") + key + "\"";
        size_t p = s.find(needle, start);
        if (p == std::string::npos) return 0;
        p = s.find(':', p);
        if (p == std::string::npos) return 0;
        ++p;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
        bool neg = false;
        if (p < s.size() && s[p] == '-') { neg = true; ++p; }
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return 0;
        long long v = 0;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
            v = v * 10 + (s[p] - '0'); ++p;
        }
        out = static_cast<T>(neg ? -v : v);
        return 1;
    }
    static int json_find_int_(const std::string& s, const char* key, size_t start, uint32_t& out) {
        return json_find_int_gen_(s, key, start, out);
    }
    static int json_find_int64_(const std::string& s, const char* key, size_t start, int64_t& out) {
        return json_find_int_gen_(s, key, start, out);
    }

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, TableSchema> tables_;
    uint16_t next_table_id_ = 1;  // 0 reserved for legacy/default; never reused on drop
};

} // namespace zeptodb::storage

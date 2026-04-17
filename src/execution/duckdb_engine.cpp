// ============================================================================
// ZeptoDB: Embedded DuckDB Engine Implementation
// ============================================================================

#ifdef ZEPTO_ENABLE_DUCKDB

#include "zeptodb/execution/duckdb_engine.h"
#include "zeptodb/common/logger.h"

#include <climits>
#include <limits>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "duckdb.hpp"
#pragma GCC diagnostic pop

namespace zeptodb::execution {

// ============================================================================
// SQL escape helpers — prevent injection in string literals and identifiers
// ============================================================================
static std::string escape_sql_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

static std::string escape_sql_identifier(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out;
}

// ============================================================================
// Impl — pimpl to isolate DuckDB headers
// ============================================================================
struct DuckDBEngine::Impl {
    std::unique_ptr<duckdb::DuckDB> db;

    explicit Impl(const DuckDBConfig& cfg) {
        duckdb::DBConfig db_config;
        if (cfg.memory_limit_mb > 0) {
            db_config.SetOption("memory_limit",
                                duckdb::Value(std::to_string(cfg.memory_limit_mb) + "MB"));
        }
        if (cfg.threads > 0) {
            db_config.SetOption("threads", duckdb::Value::INTEGER(cfg.threads));
        }
        if (!cfg.temp_directory.empty()) {
            db_config.SetOption("temp_directory", duckdb::Value(cfg.temp_directory));
        }
        db = std::make_unique<duckdb::DuckDB>(nullptr, &db_config);
    }
};

// ============================================================================
// DuckDBEngine
// ============================================================================

DuckDBEngine::DuckDBEngine(const DuckDBConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
    ZEPTO_INFO("DuckDBEngine: initialized (memory_limit={}MB, threads={})",
               config.memory_limit_mb, config.threads);
}

DuckDBEngine::~DuckDBEngine() = default;

bool DuckDBEngine::is_initialized() const {
    return impl_ && impl_->db;
}

DuckDBResult DuckDBEngine::execute(const std::string& sql) {
    DuckDBResult result;
    try {
        auto conn = std::make_unique<duckdb::Connection>(*impl_->db);
        auto qr = conn->Query(sql);

        if (qr->HasError()) {
            result.error = qr->GetError();
            return result;
        }

        result.num_columns = qr->ColumnCount();
        for (duckdb::idx_t i = 0; i < result.num_columns; ++i) {
            result.column_names.push_back(qr->ColumnName(i));
        }

        // Determine column type hints from DuckDB result types
        result.int_columns.resize(result.num_columns);
        result.dbl_columns.resize(result.num_columns);
        result.str_columns.resize(result.num_columns);
        result.column_type_hints.resize(result.num_columns, 0);

        for (duckdb::idx_t i = 0; i < result.num_columns; ++i) {
            auto t = qr->types[i].id();
            if (t == duckdb::LogicalTypeId::FLOAT || t == duckdb::LogicalTypeId::DOUBLE)
                result.column_type_hints[i] = 1;
            else if (t == duckdb::LogicalTypeId::VARCHAR)
                result.column_type_hints[i] = 2;
            else if (t == duckdb::LogicalTypeId::TIMESTAMP || t == duckdb::LogicalTypeId::TIMESTAMP_TZ)
                result.column_type_hints[i] = 3;  // us → ns (*1000)
            else if (t == duckdb::LogicalTypeId::TIMESTAMP_MS)
                result.column_type_hints[i] = 4;  // ms → ns (*1000000)
            else if (t == duckdb::LogicalTypeId::TIMESTAMP_SEC)
                result.column_type_hints[i] = 5;  // s → ns (*1000000000)
            // TIMESTAMP_NS falls through to default (0) = plain int64, already ns
        }

        // Materialize all chunks
        size_t total_rows = 0;
        while (auto chunk = qr->Fetch()) {
            if (chunk->size() == 0) break;
            size_t chunk_rows = chunk->size();
            for (duckdb::idx_t c = 0; c < result.num_columns; ++c) {
                auto& vec = chunk->data[c];
                switch (result.column_type_hints[c]) {
                    case 1: // double
                        for (duckdb::idx_t r = 0; r < chunk_rows; ++r) {
                            auto val = vec.GetValue(r);
                            result.dbl_columns[c].push_back(
                                val.IsNull() ? std::numeric_limits<double>::quiet_NaN()
                                             : val.GetValue<double>());
                        }
                        break;
                    case 2: // string
                        for (duckdb::idx_t r = 0; r < chunk_rows; ++r) {
                            auto val = vec.GetValue(r);
                            result.str_columns[c].push_back(
                                val.IsNull() ? "" : val.ToString());
                        }
                        break;
                    case 3: case 4: case 5: { // timestamp → nanoseconds
                        int64_t mul = (result.column_type_hints[c] == 3) ? 1000LL :
                                     (result.column_type_hints[c] == 4) ? 1000000LL : 1000000000LL;
                        for (duckdb::idx_t r = 0; r < chunk_rows; ++r) {
                            auto val = vec.GetValue(r);
                            result.int_columns[c].push_back(
                                val.IsNull() ? INT64_MIN : val.GetValue<int64_t>() * mul);
                        }
                        break;
                    }
                    default: // int64
                        for (duckdb::idx_t r = 0; r < chunk_rows; ++r) {
                            auto val = vec.GetValue(r);
                            result.int_columns[c].push_back(
                                val.IsNull() ? INT64_MIN : val.GetValue<int64_t>());
                        }
                        break;
                }
            }
            total_rows += chunk_rows;
        }
        result.num_rows = total_rows;

    } catch (const std::exception& e) {
        result.error = std::string("DuckDB error: ") + e.what();
        ZEPTO_WARN("DuckDBEngine::execute failed: {}", e.what());
    }
    return result;
}

bool DuckDBEngine::register_parquet(const std::string& table_name,
                                     const std::string& parquet_path) {
    try {
        auto conn = std::make_unique<duckdb::Connection>(*impl_->db);
        std::string sql = "CREATE OR REPLACE VIEW \"" + escape_sql_identifier(table_name) +
                          "\" AS SELECT * FROM read_parquet('" + escape_sql_string(parquet_path) + "')";
        auto result = conn->Query(sql);
        if (result->HasError()) {
            ZEPTO_WARN("DuckDBEngine: register_parquet failed: {}", result->GetError());
            return false;
        }
        ZEPTO_INFO("DuckDBEngine: registered parquet '{}' as '{}'", parquet_path, table_name);
        return true;
    } catch (const std::exception& e) {
        ZEPTO_WARN("DuckDBEngine: register_parquet exception: {}", e.what());
        return false;
    }
}

} // namespace zeptodb::execution

#endif // ZEPTO_ENABLE_DUCKDB

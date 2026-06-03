#pragma once
// ============================================================================
// ZeptoDB: Telegraf external output helpers
// ============================================================================
// Parses Telegraf's Influx line protocol stream and builds ZeptoDB SQL INSERT
// batches for the `outputs.execd` external plugin path.
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zeptodb::feeds {

enum class TelegrafTimestampUnit {
    Nanoseconds,
    Microseconds,
    Milliseconds,
    Seconds,
};

struct TelegrafFieldValue {
    enum class Type {
        Integer,
        Unsigned,
        Float,
        Boolean,
        String,
    };

    Type type = Type::String;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0.0;
    bool b = false;
    std::string s;
};

struct TelegrafMetric {
    std::string measurement;
    std::unordered_map<std::string, std::string> tags;
    std::unordered_map<std::string, TelegrafFieldValue> fields;
    std::optional<int64_t> timestamp;
};

struct TelegrafOutputConfig {
    std::string table_name = "telegraf";
    std::string symbol_tag = "symbol";
    std::string price_field = "value";
    std::string volume_field = "volume";
    int64_t default_volume = 1;
    double price_scale = 1.0;
    double volume_scale = 1.0;
    TelegrafTimestampUnit timestamp_unit = TelegrafTimestampUnit::Nanoseconds;
    bool measurement_as_symbol = true;
};

struct TelegrafSqlRow {
    std::string symbol;
    int64_t price = 0;
    int64_t volume = 0;
    int64_t timestamp_ns = 0;
};

struct TelegrafBuildResult {
    std::string sql;
    size_t rows = 0;
    std::vector<std::string> errors;
};

/// Parse one Influx line protocol metric emitted by Telegraf serializers.
/// Comments, empty lines, malformed tags/fields, non-finite numbers, and
/// invalid timestamps return std::nullopt and optionally fill `error`.
std::optional<TelegrafMetric> parse_telegraf_line(std::string_view line,
                                                  std::string* error = nullptr);

/// Map a parsed Telegraf metric to ZeptoDB's canonical tick columns:
/// `(symbol, price, volume, timestamp)`. Numeric fields are scaled and rounded
/// to int64. Missing timestamps map to 0; configure Telegraf to emit ns
/// timestamps for production ingest.
std::optional<TelegrafSqlRow> metric_to_telegraf_sql_row(
    const TelegrafMetric& metric,
    const TelegrafOutputConfig& config,
    std::string* error = nullptr);

/// Build one multi-row SQL INSERT statement for the configured table. The
/// table name is validated as a simple SQL identifier and symbol strings are
/// SQL-escaped.
TelegrafBuildResult build_telegraf_insert_sql(
    const std::vector<TelegrafSqlRow>& rows,
    const TelegrafOutputConfig& config);

/// Return true when `table_name` is safe to splice as an unquoted SQL
/// identifier: `[A-Za-z_][A-Za-z0-9_]*`.
bool is_valid_telegraf_table_name(std::string_view table_name);
std::string escape_telegraf_sql_string(std::string_view value);

std::optional<TelegrafTimestampUnit> parse_telegraf_timestamp_unit(
    std::string_view value);

} // namespace zeptodb::feeds

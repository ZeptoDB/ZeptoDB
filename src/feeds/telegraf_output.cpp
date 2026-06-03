// ============================================================================
// ZeptoDB: Telegraf external output helpers
// ============================================================================

#include "zeptodb/feeds/telegraf_output.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace zeptodb::feeds {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

size_t find_unescaped_space(std::string_view s, size_t begin) {
    bool escaped = false;
    bool in_quotes = false;
    for (size_t i = begin; i < s.size(); ++i) {
        const char c = s[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && c == ' ') {
            return i;
        }
    }
    return std::string_view::npos;
}

std::vector<std::string_view> split_unescaped(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    bool escaped = false;
    bool in_quotes = false;
    size_t start = 0;

    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && c == delim) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }

    out.push_back(s.substr(start));
    return out;
}

size_t find_unescaped(std::string_view s, char needle) {
    bool escaped = false;
    bool in_quotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && c == needle) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::string unescape_key(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool escaped = false;
    for (const char c : s) {
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) {
        out.push_back('\\');
    }
    return out;
}

bool parse_i64(std::string_view s, int64_t* out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, *out);
    return ec == std::errc{} && ptr == last;
}

bool parse_u64(std::string_view s, uint64_t* out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, *out);
    return ec == std::errc{} && ptr == last;
}

bool parse_double(std::string_view s, double* out) {
    if (s.empty()) return false;
    std::string tmp(s);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || errno == ERANGE || !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

std::optional<std::string> parse_quoted_string(std::string_view s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') {
        return std::nullopt;
    }
    std::string out;
    out.reserve(s.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        const char c = s[i];
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) {
        return std::nullopt;
    }
    return out;
}

std::optional<TelegrafFieldValue> parse_field_value(std::string_view raw) {
    raw = trim(raw);
    if (raw.empty()) return std::nullopt;

    if (raw.front() == '"') {
        auto s = parse_quoted_string(raw);
        if (!s) return std::nullopt;
        TelegrafFieldValue v;
        v.type = TelegrafFieldValue::Type::String;
        v.s = std::move(*s);
        return v;
    }

    std::string lower(raw);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "t" || lower == "true" || lower == "f" || lower == "false") {
        TelegrafFieldValue v;
        v.type = TelegrafFieldValue::Type::Boolean;
        v.b = (lower == "t" || lower == "true");
        return v;
    }

    if (raw.back() == 'i') {
        int64_t value = 0;
        if (!parse_i64(raw.substr(0, raw.size() - 1), &value)) return std::nullopt;
        TelegrafFieldValue v;
        v.type = TelegrafFieldValue::Type::Integer;
        v.i = value;
        return v;
    }

    if (raw.back() == 'u') {
        uint64_t value = 0;
        if (!parse_u64(raw.substr(0, raw.size() - 1), &value)) return std::nullopt;
        TelegrafFieldValue v;
        v.type = TelegrafFieldValue::Type::Unsigned;
        v.u = value;
        return v;
    }

    double value = 0.0;
    if (!parse_double(raw, &value)) return std::nullopt;
    TelegrafFieldValue v;
    v.type = TelegrafFieldValue::Type::Float;
    v.f = value;
    return v;
}

std::optional<int64_t> scale_numeric_field(const TelegrafFieldValue& value,
                                           double scale,
                                           std::string* error) {
    long double base = 0.0L;
    switch (value.type) {
        case TelegrafFieldValue::Type::Integer:
            base = static_cast<long double>(value.i);
            break;
        case TelegrafFieldValue::Type::Unsigned:
            base = static_cast<long double>(value.u);
            break;
        case TelegrafFieldValue::Type::Float:
            base = static_cast<long double>(value.f);
            break;
        case TelegrafFieldValue::Type::Boolean:
        case TelegrafFieldValue::Type::String:
            if (error) *error = "field is not numeric";
            return std::nullopt;
    }

    const long double scaled = base * static_cast<long double>(scale);
    if (!std::isfinite(static_cast<double>(scaled)) ||
        scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        scaled < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        if (error) *error = "scaled numeric field is outside int64 range";
        return std::nullopt;
    }
    return static_cast<int64_t>(std::llround(scaled));
}

std::optional<int64_t> normalize_timestamp(int64_t value,
                                           TelegrafTimestampUnit unit) {
    auto mul = [](int64_t v, int64_t factor) -> std::optional<int64_t> {
        if (v > 0 && v > std::numeric_limits<int64_t>::max() / factor) {
            return std::nullopt;
        }
        if (v < 0 && v < std::numeric_limits<int64_t>::min() / factor) {
            return std::nullopt;
        }
        return v * factor;
    };

    switch (unit) {
        case TelegrafTimestampUnit::Nanoseconds:
            return value;
        case TelegrafTimestampUnit::Microseconds:
            return mul(value, 1000LL);
        case TelegrafTimestampUnit::Milliseconds:
            return mul(value, 1000000LL);
        case TelegrafTimestampUnit::Seconds:
            return mul(value, 1000000000LL);
    }
    return value;
}

} // namespace

std::optional<TelegrafMetric> parse_telegraf_line(std::string_view line,
                                                  std::string* error) {
    line = trim(line);
    if (line.empty()) {
        if (error) *error = "empty line";
        return std::nullopt;
    }
    if (line.front() == '#') {
        if (error) *error = "comment line";
        return std::nullopt;
    }

    const size_t first_space = find_unescaped_space(line, 0);
    if (first_space == std::string_view::npos) {
        if (error) *error = "line protocol requires measurement/tags and fields";
        return std::nullopt;
    }

    TelegrafMetric metric;
    const std::string_view key_part = line.substr(0, first_space);
    std::string_view rest = trim(line.substr(first_space + 1));
    const size_t second_space = find_unescaped_space(rest, 0);
    const std::string_view field_part =
        second_space == std::string_view::npos ? rest : rest.substr(0, second_space);
    const std::string_view timestamp_part =
        second_space == std::string_view::npos ? std::string_view{} : trim(rest.substr(second_space + 1));

    const auto key_tokens = split_unescaped(key_part, ',');
    if (key_tokens.empty() || trim(key_tokens.front()).empty()) {
        if (error) *error = "measurement is empty";
        return std::nullopt;
    }
    metric.measurement = unescape_key(trim(key_tokens.front()));

    for (size_t i = 1; i < key_tokens.size(); ++i) {
        const std::string_view token = trim(key_tokens[i]);
        const size_t eq = find_unescaped(token, '=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 >= token.size()) {
            if (error) *error = "invalid tag assignment";
            return std::nullopt;
        }
        metric.tags.emplace(unescape_key(token.substr(0, eq)),
                            unescape_key(token.substr(eq + 1)));
    }

    const auto field_tokens = split_unescaped(field_part, ',');
    for (const std::string_view token_raw : field_tokens) {
        const std::string_view token = trim(token_raw);
        const size_t eq = find_unescaped(token, '=');
        if (eq == std::string_view::npos || eq == 0 || eq + 1 >= token.size()) {
            if (error) *error = "invalid field assignment";
            return std::nullopt;
        }
        auto value = parse_field_value(token.substr(eq + 1));
        if (!value) {
            if (error) *error = "invalid field value";
            return std::nullopt;
        }
        metric.fields.emplace(unescape_key(token.substr(0, eq)), std::move(*value));
    }

    if (metric.fields.empty()) {
        if (error) *error = "metric has no fields";
        return std::nullopt;
    }

    if (!timestamp_part.empty()) {
        int64_t ts = 0;
        if (!parse_i64(timestamp_part, &ts)) {
            if (error) *error = "invalid timestamp";
            return std::nullopt;
        }
        metric.timestamp = ts;
    }

    return metric;
}

std::optional<TelegrafSqlRow> metric_to_telegraf_sql_row(
    const TelegrafMetric& metric,
    const TelegrafOutputConfig& config,
    std::string* error) {
    if (metric.measurement.empty()) {
        if (error) *error = "metric measurement is empty";
        return std::nullopt;
    }

    std::string symbol;
    if (!config.symbol_tag.empty()) {
        auto it = metric.tags.find(config.symbol_tag);
        if (it != metric.tags.end()) {
            symbol = it->second;
        }
    }
    if (symbol.empty() && config.measurement_as_symbol) {
        symbol = metric.measurement;
    }
    if (symbol.empty()) {
        if (error) *error = "symbol tag is missing";
        return std::nullopt;
    }

    auto price_it = metric.fields.find(config.price_field);
    if (price_it == metric.fields.end()) {
        if (error) *error = "price field '" + config.price_field + "' is missing";
        return std::nullopt;
    }

    std::string numeric_error;
    auto price = scale_numeric_field(price_it->second, config.price_scale, &numeric_error);
    if (!price) {
        if (error) *error = "price " + numeric_error;
        return std::nullopt;
    }

    int64_t volume = config.default_volume;
    auto volume_it = metric.fields.find(config.volume_field);
    if (volume_it != metric.fields.end()) {
        auto scaled_volume = scale_numeric_field(volume_it->second,
                                                 config.volume_scale,
                                                 &numeric_error);
        if (!scaled_volume) {
            if (error) *error = "volume " + numeric_error;
            return std::nullopt;
        }
        volume = *scaled_volume;
    }

    TelegrafSqlRow row;
    row.symbol = std::move(symbol);
    row.price = *price;
    row.volume = volume;
    if (metric.timestamp.has_value()) {
        auto ts = normalize_timestamp(*metric.timestamp, config.timestamp_unit);
        if (!ts) {
            if (error) *error = "timestamp conversion is outside int64 ns range";
            return std::nullopt;
        }
        row.timestamp_ns = *ts;
    } else {
        row.timestamp_ns = 0;
    }
    return row;
}

TelegrafBuildResult build_telegraf_insert_sql(
    const std::vector<TelegrafSqlRow>& rows,
    const TelegrafOutputConfig& config) {
    TelegrafBuildResult result;
    if (rows.empty()) {
        return result;
    }
    if (!is_valid_telegraf_table_name(config.table_name)) {
        result.errors.push_back("invalid table name '" + config.table_name + "'");
        return result;
    }

    std::ostringstream os;
    os << "INSERT INTO " << config.table_name
       << " (symbol, price, volume, timestamp) VALUES ";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) os << ", ";
        const auto& row = rows[i];
        os << "('" << escape_telegraf_sql_string(row.symbol) << "', "
           << row.price << ", " << row.volume << ", " << row.timestamp_ns << ")";
    }
    result.sql = os.str();
    result.rows = rows.size();
    return result;
}

bool is_valid_telegraf_table_name(std::string_view table_name) {
    if (table_name.empty()) return false;
    const auto is_first = [](unsigned char c) {
        return std::isalpha(c) || c == '_';
    };
    const auto is_rest = [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    };
    if (!is_first(static_cast<unsigned char>(table_name.front()))) {
        return false;
    }
    for (const char c : table_name.substr(1)) {
        if (!is_rest(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::string escape_telegraf_sql_string(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<TelegrafTimestampUnit> parse_telegraf_timestamp_unit(
    std::string_view value) {
    if (value == "ns" || value == "nanosecond" || value == "nanoseconds") {
        return TelegrafTimestampUnit::Nanoseconds;
    }
    if (value == "us" || value == "microsecond" || value == "microseconds") {
        return TelegrafTimestampUnit::Microseconds;
    }
    if (value == "ms" || value == "millisecond" || value == "milliseconds") {
        return TelegrafTimestampUnit::Milliseconds;
    }
    if (value == "s" || value == "sec" || value == "second" || value == "seconds") {
        return TelegrafTimestampUnit::Seconds;
    }
    return std::nullopt;
}

} // namespace zeptodb::feeds

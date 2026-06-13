// ============================================================================
// ZeptoDB: Telegraf outputs.execd external output
// ============================================================================

#include "third_party/httplib.h"
#include "zeptodb/feeds/telegraf_output.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using zeptodb::feeds::TelegrafOutputConfig;
using zeptodb::feeds::TelegrafSqlRow;

struct CliConfig {
    std::string url = "http://127.0.0.1:8123";
    std::string auth_token;
    std::string tenant_id;
    size_t batch_size = 1;
    bool fail_on_parse_error = false;
    TelegrafOutputConfig output;
};

const char* env_or_null(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Reads Telegraf Influx line protocol from stdin and writes SQL INSERT\n"
        << "batches to ZeptoDB's HTTP endpoint. Intended for Telegraf outputs.execd.\n"
        << "\n"
        << "Options:\n"
        << "  --url URL                 ZeptoDB HTTP URL (default: http://127.0.0.1:8123)\n"
        << "  --table NAME              Destination table (default: telegraf)\n"
        << "  --auth-token TOKEN        Bearer token; env ZEPTO_TELEGRAF_TOKEN or ZEPTO_API_KEY also works\n"
        << "  --tenant ID               X-Zepto-Tenant-Id header for no-auth tenant-scoped deployments\n"
        << "  --symbol-tag TAG          Tag used as ZeptoDB symbol (default: symbol)\n"
        << "  --no-measurement-symbol   Do not fall back to measurement as symbol\n"
        << "  --price-field FIELD       Numeric field used as price/value (default: value)\n"
        << "  --volume-field FIELD      Numeric field used as volume (default: volume)\n"
        << "  --default-volume N        Volume when volume field is absent (default: 1)\n"
        << "  --price-scale N           Multiplier before int64 price storage (default: 1)\n"
        << "  --volume-scale N          Multiplier before int64 volume storage (default: 1)\n"
        << "  --timestamp-unit UNIT     ns, us, ms, or s (default: ns)\n"
        << "  --batch-size N            Lines per HTTP INSERT batch (default: 1)\n"
        << "  --fail-on-parse-error     Exit non-zero on malformed input\n"
        << "  --help                    Show this help\n";
}

bool parse_size(const std::string& s, size_t* out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || v == 0) return false;
    *out = static_cast<size_t>(v);
    return true;
}

bool parse_i64(const std::string& s, int64_t* out) {
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    *out = static_cast<int64_t>(v);
    return true;
}

bool parse_double(const std::string& s, double* out) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

bool parse_args(int argc, char** argv, CliConfig* cfg) {
    if (const char* url = env_or_null("ZEPTO_TELEGRAF_URL")) cfg->url = url;
    if (const char* table = env_or_null("ZEPTO_TELEGRAF_TABLE")) cfg->output.table_name = table;
    if (const char* token = env_or_null("ZEPTO_TELEGRAF_TOKEN")) cfg->auth_token = token;
    if (cfg->auth_token.empty()) {
        if (const char* token = env_or_null("ZEPTO_API_KEY")) cfg->auth_token = token;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--url") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->url = v;
        } else if (arg == "--table") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->output.table_name = v;
        } else if (arg == "--auth-token") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->auth_token = v;
        } else if (arg == "--tenant") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->tenant_id = v;
        } else if (arg == "--symbol-tag") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->output.symbol_tag = v;
        } else if (arg == "--no-measurement-symbol") {
            cfg->output.measurement_as_symbol = false;
        } else if (arg == "--price-field") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->output.price_field = v;
        } else if (arg == "--volume-field") {
            const char* v = need_value(arg);
            if (!v) return false;
            cfg->output.volume_field = v;
        } else if (arg == "--default-volume") {
            const char* v = need_value(arg);
            if (!v || !parse_i64(v, &cfg->output.default_volume)) return false;
        } else if (arg == "--price-scale") {
            const char* v = need_value(arg);
            if (!v || !parse_double(v, &cfg->output.price_scale)) return false;
        } else if (arg == "--volume-scale") {
            const char* v = need_value(arg);
            if (!v || !parse_double(v, &cfg->output.volume_scale)) return false;
        } else if (arg == "--timestamp-unit") {
            const char* v = need_value(arg);
            if (!v) return false;
            auto unit = zeptodb::feeds::parse_telegraf_timestamp_unit(v);
            if (!unit) {
                std::cerr << "invalid timestamp unit: " << v << "\n";
                return false;
            }
            cfg->output.timestamp_unit = *unit;
        } else if (arg == "--batch-size") {
            const char* v = need_value(arg);
            if (!v || !parse_size(v, &cfg->batch_size)) return false;
        } else if (arg == "--fail-on-parse-error") {
            cfg->fail_on_parse_error = true;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            return false;
        }
    }

    if (!zeptodb::feeds::is_valid_telegraf_table_name(cfg->output.table_name)) {
        std::cerr << "invalid destination table: " << cfg->output.table_name << "\n";
        return false;
    }
    return true;
}

class ZeptoHttpWriter {
public:
    explicit ZeptoHttpWriter(const CliConfig& cfg)
        : cfg_(cfg), client_(cfg.url) {
        client_.set_connection_timeout(5);
        client_.set_read_timeout(30);
        client_.set_write_timeout(30);
    }

    bool send_sql(const std::string& sql) {
        httplib::Headers headers;
        headers.emplace("Content-Type", "text/plain");
        if (!cfg_.auth_token.empty()) {
            headers.emplace("Authorization", "Bearer " + cfg_.auth_token);
        }
        if (!cfg_.tenant_id.empty()) {
            headers.emplace("X-Zepto-Tenant-Id", cfg_.tenant_id);
        }

        auto res = client_.Post("/", headers, sql, "text/plain");
        if (!res) {
            std::cerr << "ZeptoDB HTTP request failed\n";
            return false;
        }
        if (res->status < 200 || res->status >= 300) {
            std::cerr << "ZeptoDB HTTP " << res->status << ": " << res->body << "\n";
            return false;
        }
        return true;
    }

private:
    const CliConfig& cfg_;
    httplib::Client client_;
};

bool flush_rows(const CliConfig& cfg,
                ZeptoHttpWriter* writer,
                std::vector<TelegrafSqlRow>* rows) {
    if (rows->empty()) return true;
    auto built = zeptodb::feeds::build_telegraf_insert_sql(*rows, cfg.output);
    rows->clear();
    if (!built.errors.empty()) {
        for (const auto& err : built.errors) {
            std::cerr << "build error: " << err << "\n";
        }
        return false;
    }
    return writer->send_sql(built.sql);
}

} // namespace

int main(int argc, char** argv) {
    CliConfig cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 2;
    }

    ZeptoHttpWriter writer(cfg);
    std::vector<TelegrafSqlRow> rows;
    rows.reserve(cfg.batch_size);

    size_t accepted = 0;
    size_t dropped = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string error;
        auto metric = zeptodb::feeds::parse_telegraf_line(line, &error);
        if (!metric) {
            ++dropped;
            std::cerr << "parse error: " << error << "\n";
            if (cfg.fail_on_parse_error) return 3;
            continue;
        }

        auto row = zeptodb::feeds::metric_to_telegraf_sql_row(*metric, cfg.output, &error);
        if (!row) {
            ++dropped;
            std::cerr << "mapping error: " << error << "\n";
            if (cfg.fail_on_parse_error) return 3;
            continue;
        }

        rows.push_back(std::move(*row));
        ++accepted;
        if (rows.size() >= cfg.batch_size) {
            if (!flush_rows(cfg, &writer, &rows)) return 4;
        }
    }

    if (!flush_rows(cfg, &writer, &rows)) return 4;
    std::cerr << "zepto-telegraf-output accepted=" << accepted
              << " dropped=" << dropped << "\n";
    return 0;
}

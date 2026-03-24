// ============================================================================
// ZeptoDB Migration Tool: zepto-migrate
// ============================================================================
#include "zeptodb/migration/q_parser.h"
#include "zeptodb/migration/q_to_python.h"
#include "zeptodb/migration/hdb_loader.h"
#include "zeptodb/migration/clickhouse_migrator.h"
#include "zeptodb/migration/duckdb_interop.h"
#include "zeptodb/migration/timescaledb_migrator.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;
using namespace zeptodb::migration;

// ============================================================================
// Command Line Interface
// ============================================================================

enum class MigrateMode {
    QUERY,          // Convert q query to APEX SQL
    HDB,            // Load kdb+ HDB data into APEX
    CLICKHOUSE,     // Migrate to ClickHouse
    DUCKDB,         // Export to DuckDB/Parquet
    TIMESCALEDB,    // Migrate to TimescaleDB
    VALIDATE,       // Validate migration
    HELP
};

struct MigrateOptions {
    MigrateMode mode = MigrateMode::HELP;
    std::string input;
    std::string output;
    std::string table_name;
    std::string partition_date;
    bool verbose = false;
    bool dry_run = false;
    std::string target = "sql";  // sql or python

    // ClickHouse options
    std::string ch_host = "localhost";
    int ch_port = 8123;
    std::string ch_db = "default";

    // TimescaleDB options
    std::string pg_conn;
    bool enable_compression = true;
    bool add_retention = false;
    int retention_days = 365;

    // DuckDB options
    std::string parquet_format = "SNAPPY";
    int duckdb_threads = 0;
};

void print_usage() {
    std::cout << R"(ZeptoDB Migration Tool
======================

Usage: zepto-migrate <mode> [options]

Modes:
  query        Convert kdb+ q query to ZeptoDB SQL
  hdb          Load kdb+ HDB data into ZeptoDB
  clickhouse   Migrate ZeptoDB data to ClickHouse
  duckdb       Export ZeptoDB data to Parquet (DuckDB compatible)
  timescaledb  Generate TimescaleDB migration SQL
  validate     Validate migration accuracy
  help         Show this help

Query Mode:
  -i, --input <file>       Input .q file
  -o, --output <file>      Output file (default: stdout)
  --target <sql|python>    Output target (default: sql)

HDB Mode:
  -d, --hdb-dir <path>     kdb+ HDB root directory
  -o, --output <path>      ZeptoDB output directory
  -t, --table <name>       Specific table (default: all)
  -p, --partition <date>   Specific partition (YYYY.MM.DD)
  --dry-run                Scan only, no data migration

ClickHouse Mode:
  -d, --hdb-dir <path>     kdb+ HDB source directory
  -o, --output <path>      Output directory for DDL/data files
  --ch-host <host>         ClickHouse host (default: localhost)
  --ch-port <port>         ClickHouse port (default: 8123)
  --ch-db <name>           ClickHouse database (default: default)
  -t, --table <name>       Specific table (default: all)

DuckDB Mode:
  -d, --hdb-dir <path>     Source data directory
  -o, --output <path>      Parquet output directory
  -t, --table <name>       Specific table (default: all)
  --compression <codec>    Parquet compression: SNAPPY|ZSTD|GZIP (default: SNAPPY)
  --threads <n>            DuckDB thread count (default: auto)

TimescaleDB Mode:
  -d, --hdb-dir <path>     kdb+ HDB source directory
  -o, --output <file>      Output SQL file (default: stdout)
  --pg-conn <conn>         PostgreSQL connection string
  -t, --table <name>       Specific table (default: trades,quotes)
  --no-compression         Disable compression policy
  --retention <days>       Add retention policy (days)

Examples:
  # Convert q query
  zepto-migrate query -i vwap.q -o vwap.sql

  # Load HDB
  zepto-migrate hdb -d /data/hdb -o /data/apex --dry-run -v

  # Generate ClickHouse DDL
  zepto-migrate clickhouse -d /data/hdb -o /tmp/ch_export --ch-db hft

  # Export to Parquet for DuckDB
  zepto-migrate duckdb -d /data/apex -o /data/parquet --compression ZSTD

  # Generate TimescaleDB setup SQL
  zepto-migrate timescaledb -d /data/hdb -o setup.sql --retention 365

)";
}

MigrateOptions parse_args(int argc, char* argv[]) {
    MigrateOptions opts;

    if (argc < 2) return opts;

    std::string mode_str = argv[1];
    if      (mode_str == "query")       opts.mode = MigrateMode::QUERY;
    else if (mode_str == "hdb")         opts.mode = MigrateMode::HDB;
    else if (mode_str == "clickhouse")  opts.mode = MigrateMode::CLICKHOUSE;
    else if (mode_str == "duckdb")      opts.mode = MigrateMode::DUCKDB;
    else if (mode_str == "timescaledb") opts.mode = MigrateMode::TIMESCALEDB;
    else if (mode_str == "validate")    opts.mode = MigrateMode::VALIDATE;
    else    { opts.mode = MigrateMode::HELP; return opts; }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if      (arg == "-i" || arg == "--input")       { if (i+1 < argc) opts.input = argv[++i]; }
        else if (arg == "-o" || arg == "--output")      { if (i+1 < argc) opts.output = argv[++i]; }
        else if (arg == "-d" || arg == "--hdb-dir")     { if (i+1 < argc) opts.input = argv[++i]; }
        else if (arg == "-t" || arg == "--table")       { if (i+1 < argc) opts.table_name = argv[++i]; }
        else if (arg == "-p" || arg == "--partition")   { if (i+1 < argc) opts.partition_date = argv[++i]; }
        else if (arg == "--ch-host")                    { if (i+1 < argc) opts.ch_host = argv[++i]; }
        else if (arg == "--ch-port")                    { if (i+1 < argc) opts.ch_port = std::stoi(argv[++i]); }
        else if (arg == "--ch-db")                      { if (i+1 < argc) opts.ch_db = argv[++i]; }
        else if (arg == "--pg-conn")                    { if (i+1 < argc) opts.pg_conn = argv[++i]; }
        else if (arg == "--compression")                { if (i+1 < argc) opts.parquet_format = argv[++i]; }
        else if (arg == "--threads")                    { if (i+1 < argc) opts.duckdb_threads = std::stoi(argv[++i]); }
        else if (arg == "--retention")                  { if (i+1 < argc) { opts.add_retention = true; opts.retention_days = std::stoi(argv[++i]); } }
        else if (arg == "--no-compression")             { opts.enable_compression = false; }
        else if (arg == "--target")                     { if (i+1 < argc) opts.target = argv[++i]; }
        else if (arg == "-v" || arg == "--verbose")     { opts.verbose = true; }
        else if (arg == "--dry-run")                    { opts.dry_run = true; }
    }

    return opts;
}

// ============================================================================
// q Query Migration
// ============================================================================

bool migrate_query(const MigrateOptions& opts) {
    std::ifstream input_file(opts.input);
    if (!input_file) {
        std::cerr << "Error: Cannot open input file: " << opts.input << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << input_file.rdbuf();
    std::string q_query = buffer.str();

    if (opts.verbose) {
        std::cout << "Parsing: " << opts.input << " (" << q_query.size() << " bytes)\n";
    }

    try {
        std::string result;

        if (opts.target == "python") {
            QToPythonTransformer transformer;
            result = transformer.transform_script(q_query);
        } else {
            QLexer lexer(q_query);
            auto tokens = lexer.tokenize();
            QParser parser(tokens);
            auto ast = parser.parse();
            QToSQLTransformer transformer;
            result = transformer.transform(ast);
        }

        if (opts.output.empty()) {
            std::cout << result << std::endl;
        } else {
            std::ofstream out(opts.output);
            out << result << std::endl;
            if (opts.verbose) std::cout << opts.target << " written to: " << opts.output << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return false;
    }

    return true;
}

// ============================================================================
// HDB Migration
// ============================================================================

bool migrate_hdb(const MigrateOptions& opts) {
    if (opts.input.empty()) {
        std::cerr << "Error: --hdb-dir required\n";
        return false;
    }
    if (opts.output.empty() && !opts.dry_run) {
        std::cerr << "Error: --output required\n";
        return false;
    }

    HDBLoader loader(opts.input);
    if (!loader.scan()) {
        std::cerr << "Error: Failed to scan HDB\n";
        return false;
    }

    if (opts.verbose || opts.dry_run) {
        for (const auto& table : loader.tables()) {
            std::cout << "Table: " << table.name
                      << " (" << table.partitions.size() << " partitions, "
                      << table.schema.size() << " columns)\n";
            if (opts.verbose) {
                for (const auto& col : table.schema) {
                    std::cout << "  " << col.name
                              << " type=" << static_cast<int>(col.type)
                              << " rows=" << col.row_count << "\n";
                }
            }
        }
        if (opts.dry_run) {
            std::cout << "\nDry run complete.\n";
            return true;
        }
    }

    const auto& tables = loader.tables();
    if (opts.table_name.empty()) {
        for (const auto& t : tables) {
            std::cout << "Migrating " << t.name << "... ";
            std::cout.flush();
            std::cout << (loader.export_to_zepto(t.name, opts.output) ? "OK" : "FAILED") << "\n";
        }
    } else {
        std::cout << "Migrating " << opts.table_name << "... ";
        std::cout.flush();
        if (!loader.export_to_zepto(opts.table_name, opts.output)) {
            std::cout << "FAILED\n";
            return false;
        }
        std::cout << "OK\n";
    }

    return true;
}

// ============================================================================
// ClickHouse Migration
// ============================================================================

bool migrate_clickhouse(const MigrateOptions& opts) {
    if (opts.input.empty()) {
        std::cerr << "Error: --hdb-dir required\n";
        return false;
    }

    ClickHouseMigrator::Config config;
    config.source_hdb_path = opts.input;
    config.clickhouse_host = opts.ch_host;
    config.clickhouse_port = opts.ch_port;
    config.clickhouse_db   = opts.ch_db;
    config.migrate_data    = !opts.dry_run;

    if (!opts.table_name.empty()) {
        config.tables = {opts.table_name};
    }

    ClickHouseMigrator migrator(config);

    if (opts.dry_run) {
        // Just generate DDL
        ClickHouseSchemaGenerator gen;
        std::vector<std::string> tables = config.tables.empty()
            ? std::vector<std::string>{"trades", "quotes"}
            : config.tables;

        for (const auto& t : tables) {
            CHTableSchema schema;
            if (t == "trades")      schema = gen.generate_trades_schema(t);
            else if (t == "quotes") schema = gen.generate_quotes_schema(t);
            else                    schema = gen.generate_trades_schema(t);

            std::string ddl = schema.to_create_ddl();

            if (!opts.output.empty()) {
                std::ofstream out(opts.output + "/" + t + ".sql");
                out << ddl << "\n";
                if (opts.verbose) std::cout << "DDL written: " << t << ".sql\n";
            } else {
                std::cout << ddl << "\n\n";
            }
        }
        return true;
    }

    return migrator.run();
}

// ============================================================================
// DuckDB / Parquet Export
// ============================================================================

bool migrate_duckdb(const MigrateOptions& opts) {
    if (opts.input.empty()) {
        std::cerr << "Error: --hdb-dir (source) required\n";
        return false;
    }
    if (opts.output.empty()) {
        std::cerr << "Error: --output (parquet directory) required\n";
        return false;
    }

    ParquetExporter::ExportOptions parquet_opts;
    parquet_opts.compression = opts.parquet_format;

    DuckDBIntegrator::Config config;
    config.zepto_data_path = opts.input;
    config.threads = opts.duckdb_threads;

    DuckDBIntegrator integrator(config);

    std::vector<std::string> tables;
    if (!opts.table_name.empty()) {
        tables = {opts.table_name};
    } else {
        // Default: export trades and quotes
        tables = {"trades", "quotes"};
    }

    for (const auto& t : tables) {
        std::cout << "Exporting " << t << " to Parquet... ";
        std::cout.flush();
        if (integrator.export_to_parquet(t, opts.output)) {
            std::cout << "OK\n";
        } else {
            std::cout << "FAILED\n";
        }
    }

    // Generate DuckDB setup script
    std::string setup_script = integrator.generate_setup_script(opts.output);
    std::string setup_file = opts.output + "/setup.sql";
    std::ofstream out(setup_file);
    out << setup_script;
    std::cout << "DuckDB setup script: " << setup_file << "\n";

    // Generate analytics examples
    std::string examples = integrator.generate_analytics_examples("trades");
    std::string examples_file = opts.output + "/analytics_examples.sql";
    std::ofstream ex_out(examples_file);
    ex_out << examples;
    std::cout << "Analytics examples: " << examples_file << "\n";

    return true;
}

// ============================================================================
// TimescaleDB Migration
// ============================================================================

bool migrate_timescaledb(const MigrateOptions& opts) {
    TimescaleDBMigrator::Config config;
    config.source_hdb_path = opts.input;
    config.pg_connection_string = opts.pg_conn;
    config.enable_compression = opts.enable_compression;
    config.add_retention_policy = opts.add_retention;
    config.retention_days = opts.retention_days;

    if (!opts.table_name.empty()) {
        config.tables = {opts.table_name};
    }

    TimescaleDBMigrator migrator(config);
    std::string sql = migrator.generate_migration_sql();

    if (opts.output.empty()) {
        std::cout << sql;
    } else {
        std::ofstream out(opts.output);
        if (!out) {
            std::cerr << "Error: Cannot write to: " << opts.output << "\n";
            return false;
        }
        out << sql;
        std::cout << "Migration SQL written to: " << opts.output << "\n";

        if (!opts.pg_conn.empty()) {
            std::cout << "Apply with:\n";
            std::cout << "  psql \"" << opts.pg_conn << "\" -f " << opts.output << "\n";
        }
    }

    return true;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    if (opts.mode == MigrateMode::HELP) {
        print_usage();
        return 0;
    }

    try {
        bool ok = false;
        switch (opts.mode) {
            case MigrateMode::QUERY:       ok = migrate_query(opts);       break;
            case MigrateMode::HDB:         ok = migrate_hdb(opts);         break;
            case MigrateMode::CLICKHOUSE:  ok = migrate_clickhouse(opts);  break;
            case MigrateMode::DUCKDB:      ok = migrate_duckdb(opts);      break;
            case MigrateMode::TIMESCALEDB: ok = migrate_timescaledb(opts); break;
            case MigrateMode::VALIDATE:
                std::cerr << "validate mode: not yet implemented\n";
                break;
            default:
                print_usage();
                break;
        }
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}

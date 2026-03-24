// ============================================================================
// ZeptoDB CLI: Interactive SQL REPL + Admin Tool
// ============================================================================
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <cstring>

// readline for interactive input (fallback to getline if not available)
#if __has_include(<readline/readline.h>)
#  include <readline/readline.h>
#  include <readline/history.h>
#  define HAVE_READLINE 1
#else
#  define HAVE_READLINE 0
#endif

// Simple HTTP client for talking to zepto_server
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

// ============================================================================
// Configuration
// ============================================================================

struct CLIConfig {
    std::string host = "localhost";
    int port = 8123;
    std::string database = "default";
    std::string format = "pretty";    // pretty | csv | json | tsv
    bool multiline = true;
    bool timing = true;
    bool verbose = false;
    std::string history_file = "~/.zepto_history";
    int connect_timeout_ms = 3000;
    std::string script_file;          // -f <file>
};

// ============================================================================
// HTTP Client (minimal, no deps)
// ============================================================================

struct HTTPResponse {
    int status = 0;
    std::string body;
    bool ok() const { return status == 200; }
};

class SimpleHTTPClient {
public:
    SimpleHTTPClient(const std::string& host, int port)
        : host_(host), port_(port) {}

    HTTPResponse post(const std::string& path, const std::string& body) {
        HTTPResponse resp;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { resp.status = -1; return resp; }

        struct hostent* he = gethostbyname(host_.c_str());
        if (!he) { ::close(fd); resp.status = -1; return resp; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd); resp.status = -2; return resp;
        }

        // Send HTTP request
        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n";
        req << "Host: " << host_ << ":" << port_ << "\r\n";
        req << "Content-Type: text/plain\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
        req << "Connection: close\r\n\r\n";
        req << body;

        std::string req_str = req.str();
        send(fd, req_str.c_str(), req_str.size(), 0);

        // Read response
        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
            raw.append(buf, n);
        }
        ::close(fd);

        // Parse status
        if (raw.size() > 12) {
            resp.status = std::stoi(raw.substr(9, 3));
        }

        // Extract body (after \r\n\r\n)
        auto pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos) {
            resp.body = raw.substr(pos + 4);
        }

        return resp;
    }

    HTTPResponse get(const std::string& path) {
        return post_raw("GET " + path + " HTTP/1.1\r\n"
                       "Host: " + host_ + ":" + std::to_string(port_) + "\r\n"
                       "Connection: close\r\n\r\n");
    }

private:
    std::string host_;
    int port_;

    HTTPResponse post_raw(const std::string& request) {
        HTTPResponse resp;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { resp.status = -1; return resp; }

        struct hostent* he = gethostbyname(host_.c_str());
        if (!he) { ::close(fd); resp.status = -1; return resp; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(fd); resp.status = -2; return resp;
        }

        send(fd, request.c_str(), request.size(), 0);

        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
        ::close(fd);

        if (raw.size() > 12) resp.status = std::stoi(raw.substr(9, 3));
        auto pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos) resp.body = raw.substr(pos + 4);
        return resp;
    }
};

// ============================================================================
// JSON/Table Formatter
// ============================================================================

// Simple JSON value parser for response formatting
struct Column {
    std::string name;
    std::vector<std::string> values;
};

std::vector<Column> parse_json_response(const std::string& json) {
    std::vector<Column> cols;

    // Find "columns": [...]
    auto col_start = json.find("\"columns\":");
    if (col_start == std::string::npos) return cols;

    auto arr_start = json.find('[', col_start);
    auto arr_end   = json.find(']', arr_start);
    if (arr_start == std::string::npos) return cols;

    // Parse column names
    std::string col_str = json.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t p = 0;
    while ((p = col_str.find('"', p)) != std::string::npos) {
        auto end = col_str.find('"', p + 1);
        cols.push_back({col_str.substr(p + 1, end - p - 1), {}});
        p = end + 1;
    }

    if (cols.empty()) return cols;

    // Find "data": [[...], ...]
    auto data_start = json.find("\"data\":");
    if (data_start == std::string::npos) return cols;

    // Parse row data — simple tokenizer
    auto rows_start = json.find('[', json.find('[', data_start) + 1);
    if (rows_start == std::string::npos) return cols;

    size_t depth = 0;
    size_t row_start = std::string::npos;
    size_t col_idx = 0;

    for (size_t i = data_start; i < json.size(); ++i) {
        if (json[i] == '[') {
            ++depth;
            if (depth == 2) { row_start = i + 1; col_idx = 0; }
        } else if (json[i] == ']') {
            if (depth == 2 && row_start != std::string::npos) {
                // End of row — already parsed via comma splitting
            }
            --depth;
            if (depth == 0) break;
        } else if (depth == 2 && json[i] == ',') {
            ++col_idx;
        }
    }

    // Simpler: scan for rows as [...] inside outer array
    size_t pos = json.find("\"data\":");
    pos = json.find("[[", pos);
    if (pos == std::string::npos) return cols;

    pos++; // skip first [
    while (pos < json.size()) {
        // Find start of row
        auto rs = json.find('[', pos);
        if (rs == std::string::npos) break;
        auto re = json.find(']', rs);
        if (re == std::string::npos) break;

        // Check it's not the outer end
        if (json[re + 1] == ']') { break; }

        std::string row_str = json.substr(rs + 1, re - rs - 1);

        // Split by comma (naive, doesn't handle nested)
        std::vector<std::string> values;
        std::string val;
        bool in_str = false;
        for (char c : row_str) {
            if (c == '"') { in_str = !in_str; continue; }
            if (c == ',' && !in_str) {
                // trim whitespace
                while (!val.empty() && (val.front() == ' ')) val.erase(val.begin());
                while (!val.empty() && (val.back() == ' ')) val.pop_back();
                values.push_back(val);
                val.clear();
            } else {
                val += c;
            }
        }
        while (!val.empty() && (val.front() == ' ')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ')) val.pop_back();
        values.push_back(val);

        for (size_t i = 0; i < values.size() && i < cols.size(); ++i) {
            cols[i].values.push_back(values[i]);
        }

        pos = re + 1;
    }

    return cols;
}

void print_pretty(const std::vector<Column>& cols) {
    if (cols.empty()) { std::cout << "(empty result)\n"; return; }

    size_t rows = cols[0].values.size();

    // Compute column widths
    std::vector<size_t> widths;
    for (const auto& col : cols) {
        size_t w = col.name.size();
        for (const auto& v : col.values) w = std::max(w, v.size());
        widths.push_back(w + 2);
    }

    // Header separator
    auto separator = [&]() {
        std::cout << '+';
        for (auto w : widths) std::cout << std::string(w, '-') << '+';
        std::cout << '\n';
    };

    separator();
    std::cout << '|';
    for (size_t i = 0; i < cols.size(); ++i) {
        std::cout << ' ' << std::setw(widths[i] - 1) << std::left << cols[i].name << '|';
    }
    std::cout << '\n';
    separator();

    for (size_t r = 0; r < rows; ++r) {
        std::cout << '|';
        for (size_t i = 0; i < cols.size(); ++i) {
            const auto& v = r < cols[i].values.size() ? cols[i].values[r] : "";
            std::cout << ' ' << std::setw(widths[i] - 1) << std::left << v << '|';
        }
        std::cout << '\n';
    }

    separator();
    std::cout << rows << " row" << (rows != 1 ? "s" : "") << " in set\n";
}

void print_csv(const std::vector<Column>& cols) {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << cols[i].name;
    }
    std::cout << '\n';

    if (cols.empty()) return;
    for (size_t r = 0; r < cols[0].values.size(); ++r) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << ',';
            if (r < cols[i].values.size()) std::cout << cols[i].values[r];
        }
        std::cout << '\n';
    }
}

void print_tsv(const std::vector<Column>& cols) {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) std::cout << '\t';
        std::cout << cols[i].name;
    }
    std::cout << '\n';

    if (cols.empty()) return;
    for (size_t r = 0; r < cols[0].values.size(); ++r) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) std::cout << '\t';
            if (r < cols[i].values.size()) std::cout << cols[i].values[r];
        }
        std::cout << '\n';
    }
}

// ============================================================================
// Built-in Commands
// ============================================================================

struct CommandResult {
    bool handled = false;
    bool quit = false;
};

class BuiltinCommands {
public:
    BuiltinCommands(CLIConfig& cfg, SimpleHTTPClient& http)
        : cfg_(cfg), http_(http) {}

    CommandResult handle(const std::string& line) {
        CommandResult r;
        if (line.empty()) { r.handled = true; return r; }

        std::string cmd = line;
        // trim
        while (!cmd.empty() && cmd.back() == ';') cmd.pop_back();

        if (cmd == "\\q" || cmd == "exit" || cmd == "quit") {
            r.handled = r.quit = true;
            return r;
        }

        if (cmd == "\\h" || cmd == "help" || cmd == "\\?") {
            print_help();
            r.handled = true;
            return r;
        }

        if (cmd == "\\s" || cmd == "status") {
            show_status();
            r.handled = true;
            return r;
        }

        if (cmd == "\\t") {
            cfg_.timing = !cfg_.timing;
            std::cout << "Timing " << (cfg_.timing ? "ON" : "OFF") << "\n";
            r.handled = true;
            return r;
        }

        // \t <sql> — run one query with timing enabled (one-shot, toggle unchanged)
        if (cmd.size() > 3 && cmd.substr(0, 3) == "\\t ") {
            std::string sql = cmd.substr(3);
            bool saved_timing = cfg_.timing;
            cfg_.timing = true;
            execute_query(sql);
            cfg_.timing = saved_timing;
            r.handled = true;
            return r;
        }

        if (cmd.substr(0, 3) == "\\f ") {
            cfg_.format = cmd.substr(3);
            std::cout << "Format: " << cfg_.format << "\n";
            r.handled = true;
            return r;
        }

        if (cmd == "show tables" || cmd == "SHOW TABLES") {
            execute_query("SELECT name FROM system.tables");
            r.handled = true;
            return r;
        }

        if (cmd.substr(0, 8) == "describe" || cmd.substr(0, 8) == "DESCRIBE") {
            execute_query(cmd);
            r.handled = true;
            return r;
        }

        if (cmd == "\\v" || cmd == "verbose") {
            cfg_.verbose = !cfg_.verbose;
            std::cout << "Verbose " << (cfg_.verbose ? "ON" : "OFF") << "\n";
            r.handled = true;
            return r;
        }

        if (cmd == "\\nodes" || cmd == "nodes") {
            show_nodes();
            r.handled = true;
            return r;
        }

        if (cmd == "\\cluster" || cmd == "cluster") {
            show_cluster();
            r.handled = true;
            return r;
        }

        return r;
    }

    void execute_query(const std::string& sql) {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto resp = http_.post("/", sql);
        auto t1 = std::chrono::high_resolution_clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!resp.ok()) {
            if (resp.status == -2) {
                std::cerr << "Error: Cannot connect to "
                          << cfg_.host << ":" << cfg_.port << "\n";
            } else if (resp.status == -1) {
                std::cerr << "Error: Socket error\n";
            } else {
                std::cerr << "Error [" << resp.status << "]: " << resp.body << "\n";
            }
            return;
        }

        if (cfg_.verbose) {
            std::cout << "Response (" << resp.body.size() << " bytes):\n";
        }

        if (cfg_.format == "json") {
            std::cout << resp.body << "\n";
        } else {
            auto cols = parse_json_response(resp.body);

            if (cfg_.format == "csv") {
                print_csv(cols);
            } else if (cfg_.format == "tsv") {
                print_tsv(cols);
            } else {
                // pretty (default)
                print_pretty(cols);
            }
        }

        if (cfg_.timing) {
            std::cout << std::fixed << std::setprecision(2)
                      << "Time: " << ms << " ms\n";
        }
    }

private:
    CLIConfig& cfg_;
    SimpleHTTPClient& http_;

    void print_help() {
        std::cout << R"(
ZeptoDB CLI Commands
====================

SQL Queries:
  <sql>;            Execute SQL query (end with semicolon)

Built-in Commands:
  \q, exit, quit   Exit the CLI
  \h, help         Show this help
  \s, status       Show server status and statistics
  \t               Toggle query timing (currently )" << (cfg_.timing ? "ON" : "OFF") << R"()
  \t <sql>         Run a query with timing enabled (one-shot, toggle unchanged)
  \f <format>      Set output format: pretty | csv | tsv | json
  \v               Toggle verbose mode
  \nodes           Show cluster nodes and their states
  \cluster         Show cluster overview (mode, partitions, stats)

Table Commands:
  SHOW TABLES      List all tables
  DESCRIBE <table> Show table schema

Example Queries:
  SELECT * FROM trades LIMIT 10;
  SELECT sym, count(*) FROM trades GROUP BY sym;
  SELECT xbar(timestamp, 300000000000), sum(size) FROM trades
    WHERE sym='AAPL' GROUP BY 1;

Format Examples:
  \f csv    (comma-separated)
  \f tsv    (tab-separated)
  \f json   (raw JSON)
  \f pretty (default, table format)

)";
    }

    void show_status() {
        auto resp = http_.get("/ping");
        if (!resp.ok()) {
            std::cerr << "Server: OFFLINE (" << cfg_.host << ":" << cfg_.port << ")\n";
            return;
        }
        std::cout << "Server: ONLINE (" << cfg_.host << ":" << cfg_.port << ")\n";

        auto stats = http_.get("/stats");
        if (stats.ok()) {
            std::cout << "Stats:\n" << stats.body << "\n";
        }
    }

    void show_nodes() {
        auto resp = http_.get("/admin/nodes");
        if (!resp.ok()) {
            if (resp.status == -2)
                std::cerr << "Error: Cannot connect to server\n";
            else if (resp.status == 401 || resp.status == 403)
                std::cerr << "Error: Admin permission required\n";
            else
                std::cerr << "Error [" << resp.status << "]: " << resp.body << "\n";
            return;
        }
        std::cout << "Cluster Nodes:\n" << resp.body << "\n";
    }

    void show_cluster() {
        auto resp = http_.get("/admin/cluster");
        if (!resp.ok()) {
            if (resp.status == -2)
                std::cerr << "Error: Cannot connect to server\n";
            else if (resp.status == 401 || resp.status == 403)
                std::cerr << "Error: Admin permission required\n";
            else
                std::cerr << "Error [" << resp.status << "]: " << resp.body << "\n";
            return;
        }
        std::cout << "Cluster Status:\n" << resp.body << "\n";
    }
};

// ============================================================================
// REPL
// ============================================================================

class REPL {
public:
    REPL(CLIConfig& cfg) : cfg_(cfg), http_(cfg.host, cfg.port), cmds_(cfg_, http_) {}

    void run() {
#if HAVE_READLINE
        // Set up readline history
        std::string hist = expand_home(cfg_.history_file);
        read_history(hist.c_str());
        rl_attempted_completion_function = nullptr;
#endif
        print_banner();

        std::string accumulated;

        while (true) {
            std::string prompt = accumulated.empty() ? "zepto> " : "   -> ";
            std::string line = read_line(prompt);

            if (line.empty() && accumulated.empty()) continue;

            // Check for quit
            if (line == "\\q" || line == "exit" || line == "quit") break;

            // Accumulate multiline input
            accumulated += (accumulated.empty() ? "" : " ") + line;

            // Execute if we have a complete statement (ends with ;) or is a command
            if (!accumulated.empty() && (
                accumulated.back() == ';' ||
                accumulated[0] == '\\' ||
                is_keyword_command(accumulated)
            )) {
                auto result = cmds_.handle(accumulated);
                if (result.quit) break;

                if (!result.handled) {
                    cmds_.execute_query(accumulated);
                }

#if HAVE_READLINE
                add_history(accumulated.c_str());
#endif
                accumulated.clear();
            }
        }

#if HAVE_READLINE
        std::string hist = expand_home(cfg_.history_file);
        write_history(hist.c_str());
#endif
        std::cout << "Bye.\n";
    }

    void run_script(const std::string& file) {
        std::ifstream in(file);
        if (!in) {
            std::cerr << "Error: Cannot open file: " << file << "\n";
            return;
        }

        std::string line, accumulated;
        int line_num = 0;

        while (std::getline(in, line)) {
            ++line_num;

            // Skip comments
            if (line.empty() || line[0] == '-' || line.substr(0, 2) == "--") continue;

            accumulated += (accumulated.empty() ? "" : " ") + line;

            if (!accumulated.empty() && accumulated.back() == ';') {
                if (cfg_.verbose) {
                    std::cout << "-- [line " << line_num << "] " << accumulated << "\n";
                }
                cmds_.execute_query(accumulated);
                accumulated.clear();
            }
        }

        if (!accumulated.empty()) {
            cmds_.execute_query(accumulated);
        }
    }

    void run_query(const std::string& query) {
        cmds_.execute_query(query);
    }

private:
    CLIConfig& cfg_;
    SimpleHTTPClient http_;
    BuiltinCommands cmds_;

    void print_banner() {
        std::cout << "\n"
                  << "  ZeptoDB CLI  \n"
                  << "  Ultra-Low Latency Time-Series Database\n"
                  << "  Connected to " << cfg_.host << ":" << cfg_.port << "\n"
                  << "  Type \\h for help, \\q to quit\n\n";
    }

    std::string read_line(const std::string& prompt) {
#if HAVE_READLINE
        char* raw = readline(prompt.c_str());
        if (!raw) return "\\q";  // EOF
        std::string line(raw);
        free(raw);
        return line;
#else
        std::cout << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return "\\q";
        return line;
#endif
    }

    bool is_keyword_command(const std::string& s) {
        static const std::vector<std::string> keywords = {
            "show tables", "SHOW TABLES",
            "help", "status", "exit", "quit",
            "describe", "DESCRIBE"
        };
        for (const auto& kw : keywords) {
            if (s.size() >= kw.size() &&
                s.substr(0, kw.size()) == kw) return true;
        }
        return false;
    }

    std::string expand_home(const std::string& path) {
        if (path.empty() || path[0] != '~') return path;
        const char* home = getenv("HOME");
        return home ? std::string(home) + path.substr(1) : path;
    }
};

// ============================================================================
// Main
// ============================================================================

void print_usage() {
    std::cout << R"(zepto-cli — ZeptoDB Interactive SQL Shell

Usage:
  zepto-cli [options]
  zepto-cli [options] -q <query>
  zepto-cli [options] -f <script.sql>

Connection Options:
  -h, --host <host>    Server host (default: localhost)
  -p, --port <port>    Server port (default: 8123)

Output Options:
  --format <fmt>       Output format: pretty | csv | tsv | json (default: pretty)
  --no-timing          Disable query timing
  -v, --verbose        Verbose output

Execution Options:
  -q, --query <sql>    Execute single query and exit
  -f, --file <file>    Execute SQL script file and exit

Examples:
  zepto-cli                              # Interactive REPL
  zepto-cli -h 10.0.0.1 -p 8123         # Connect to remote server
  zepto-cli -q "SELECT count(*) FROM trades"
  zepto-cli -f migration.sql            # Run SQL script
  zepto-cli --format csv -q "SELECT * FROM trades LIMIT 100" > data.csv

)";
}

int main(int argc, char* argv[]) {
    CLIConfig cfg;

    std::string one_query;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-?") { print_usage(); return 0; }
        else if ((arg == "-h" || arg == "--host") && i+1 < argc)   cfg.host = argv[++i];
        else if ((arg == "-p" || arg == "--port") && i+1 < argc)   cfg.port = std::stoi(argv[++i]);
        else if ((arg == "-q" || arg == "--query") && i+1 < argc)  one_query = argv[++i];
        else if ((arg == "-f" || arg == "--file") && i+1 < argc)   cfg.script_file = argv[++i];
        else if (arg == "--format" && i+1 < argc)                  cfg.format = argv[++i];
        else if (arg == "--no-timing")                              cfg.timing = false;
        else if (arg == "-v" || arg == "--verbose")                cfg.verbose = true;
    }

    REPL repl(cfg);

    if (!one_query.empty()) {
        repl.run_query(one_query);
        return 0;
    }

    if (!cfg.script_file.empty()) {
        repl.run_script(cfg.script_file);
        return 0;
    }

    // Interactive mode
    repl.run();
    return 0;
}

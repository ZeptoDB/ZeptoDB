// ============================================================================
// APEX-DB: kdb+ HDB (Historical Database) Loader
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <filesystem>
#include <cstdint>

namespace apex::migration {

// ============================================================================
// kdb+ Data Types
// ============================================================================
enum class KType : int8_t {
    BOOL = 1,
    GUID = 2,
    BYTE = 4,
    SHORT = 5,
    INT = 6,
    LONG = 7,
    REAL = 8,
    FLOAT = 9,
    CHAR = 10,
    SYMBOL = 11,
    TIMESTAMP = 12,
    MONTH = 13,
    DATE = 14,
    DATETIME = 15,
    TIMESPAN = 16,
    MINUTE = 17,
    SECOND = 18,
    TIME = 19
};

// ============================================================================
// Column Metadata
// ============================================================================
struct HDBColumn {
    std::string name;
    KType type;
    size_t row_count;
    bool is_enumerated;
    std::string enum_domain;  // for enumerated symbols

    HDBColumn(const std::string& n, KType t, size_t rows = 0)
        : name(n), type(t), row_count(rows), is_enumerated(false) {}
};

// ============================================================================
// Partition Info
// ============================================================================
struct HDBPartition {
    std::filesystem::path path;
    std::string date;  // YYYY.MM.DD format
    size_t row_count;
    std::vector<HDBColumn> columns;

    HDBPartition(const std::filesystem::path& p, const std::string& d)
        : path(p), date(d), row_count(0) {}
};

// ============================================================================
// Table Schema
// ============================================================================
struct HDBTable {
    std::string name;
    std::vector<HDBColumn> schema;
    std::vector<HDBPartition> partitions;
    bool is_splayed;
    bool is_partitioned;

    HDBTable(const std::string& n)
        : name(n), is_splayed(false), is_partitioned(false) {}
};

// ============================================================================
// kdb+ HDB Loader
// ============================================================================
class HDBLoader {
public:
    explicit HDBLoader(const std::filesystem::path& hdb_root);

    // Scan HDB structure
    bool scan();

    // Get discovered tables
    const std::vector<HDBTable>& tables() const { return tables_; }

    // Load specific table data
    bool load_table(const std::string& table_name,
                    const std::string& partition_date = "");

    // Load column data from splayed file
    std::vector<uint8_t> load_column_data(const std::filesystem::path& column_file,
                                          KType type,
                                          size_t& row_count);

    // Symbol table handling
    std::vector<std::string> load_sym_file(const std::filesystem::path& sym_path);

    // Export to APEX-DB format
    bool export_to_apex(const std::string& table_name,
                        const std::string& output_dir);

private:
    std::filesystem::path hdb_root_;
    std::vector<HDBTable> tables_;
    std::unordered_map<std::string, std::vector<std::string>> sym_tables_;

    // Discovery methods
    bool discover_tables();
    bool discover_partitions(HDBTable& table);
    bool read_table_schema(const std::filesystem::path& table_path,
                          std::vector<HDBColumn>& schema);

    // Binary parsing
    KType read_type_byte(const uint8_t* data);
    bool is_splayed_table(const std::filesystem::path& path);

    // Type conversion
    std::string apex_type_from_ktype(KType ktype);
};

// ============================================================================
// kdb+ Binary File Reader
// ============================================================================
class KDBFileReader {
public:
    explicit KDBFileReader(const std::filesystem::path& file_path);
    ~KDBFileReader();

    bool open();
    void close();

    // Read file header
    struct Header {
        uint8_t byte_order;  // 0=big-endian, 1=little-endian
        uint8_t type;
        uint8_t attr;        // sorted/unique/grouped attributes
        uint8_t unused;
    };

    Header read_header();

    // Read data by type
    template<typename T>
    std::vector<T> read_vector(size_t count);

    std::vector<std::string> read_symbol_vector(size_t count,
                                                 const std::vector<std::string>& sym_table);

    size_t file_size() const { return file_size_; }

private:
    std::filesystem::path file_path_;
    int fd_;
    uint8_t* mapped_data_;
    size_t file_size_;
    size_t pos_;

    bool is_little_endian_;

    template<typename T>
    T read_value();

    void ensure_endianness(void* data, size_t size);
};

// ============================================================================
// APEX-DB Column Writer
// ============================================================================
class APEXColumnWriter {
public:
    APEXColumnWriter(const std::filesystem::path& output_dir,
                     const std::string& table_name);

    bool create_table(const std::vector<HDBColumn>& schema);

    template<typename T>
    bool write_column(const std::string& column_name,
                      const std::vector<T>& data);

    bool write_string_column(const std::string& column_name,
                            const std::vector<std::string>& data);

    bool finalize();

private:
    std::filesystem::path output_dir_;
    std::string table_name_;
    std::filesystem::path table_dir_;

    bool write_metadata(const std::vector<HDBColumn>& schema);
};

} // namespace apex::migration

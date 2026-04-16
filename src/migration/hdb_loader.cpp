// ============================================================================
// ZeptoDB: kdb+ HDB Loader Implementation
// ============================================================================
#include "zeptodb/migration/hdb_loader.h"
#include "zeptodb/auth/license_validator.h"
#include "zeptodb/common/logger.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

namespace zeptodb::migration {

// ============================================================================
// HDBLoader Implementation
// ============================================================================

HDBLoader::HDBLoader(const std::filesystem::path& hdb_root)
    : hdb_root_(hdb_root)
{}

bool HDBLoader::scan() {
    if (!zeptodb::auth::license().hasFeature(zeptodb::auth::Feature::MIGRATION)) {
        ZEPTO_WARN("Migration tools require Enterprise license");
        return false;
    }
    if (!std::filesystem::exists(hdb_root_)) {
        std::cerr << "HDB root does not exist: " << hdb_root_ << std::endl;
        return false;
    }

    // Load sym file first (global symbol table)
    auto sym_path = hdb_root_ / "sym";
    if (std::filesystem::exists(sym_path)) {
        sym_tables_[""] = load_sym_file(sym_path);
        std::cout << "Loaded " << sym_tables_[""].size() << " symbols from sym file" << std::endl;
    }

    return discover_tables();
}

bool HDBLoader::discover_tables() {
    // HDB structure:
    // hdb/
    //   sym              <- global symbol table
    //   2024.01.01/      <- date partition
    //     trades/        <- table (splayed)
    //       sym          <- column file
    //       time
    //       price
    //       size

    std::unordered_map<std::string, HDBTable> table_map;

    // Scan date partitions
    for (const auto& entry : std::filesystem::directory_iterator(hdb_root_)) {
        if (!entry.is_directory()) continue;

        std::string dir_name = entry.path().filename().string();

        // Check if it's a date partition (YYYY.MM.DD)
        if (dir_name.find('.') != std::string::npos &&
            dir_name.length() == 10 &&
            std::isdigit(dir_name[0])) {

            // Scan tables in this partition
            for (const auto& table_entry : std::filesystem::directory_iterator(entry.path())) {
                if (!table_entry.is_directory()) continue;

                std::string table_name = table_entry.path().filename().string();

                // Skip special directories
                if (table_name[0] == '.') continue;

                // Create or update table
                if (table_map.find(table_name) == table_map.end()) {
                    table_map[table_name] = HDBTable(table_name);
                    table_map[table_name].is_partitioned = true;
                    table_map[table_name].is_splayed = is_splayed_table(table_entry.path());
                }

                // Add partition
                HDBPartition partition(table_entry.path(), dir_name);

                // Read schema from first partition
                if (table_map[table_name].schema.empty()) {
                    read_table_schema(table_entry.path(), table_map[table_name].schema);
                }

                partition.columns = table_map[table_name].schema;
                table_map[table_name].partitions.push_back(partition);
            }
        }
    }

    // Convert map to vector
    for (auto& [name, table] : table_map) {
        tables_.push_back(std::move(table));
    }

    std::cout << "Discovered " << tables_.size() << " tables" << std::endl;
    return true;
}

bool HDBLoader::is_splayed_table(const std::filesystem::path& path) {
    // Splayed table: directory with column files
    // Check if directory contains files (not subdirectories)
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            return true;
        }
    }
    return false;
}

bool HDBLoader::read_table_schema(const std::filesystem::path& table_path,
                                  std::vector<HDBColumn>& schema) {
    // Read .d file for column names (if exists)
    auto d_file = table_path / ".d";
    std::vector<std::string> column_names;

    if (std::filesystem::exists(d_file)) {
        // .d file contains column name list
        KDBFileReader reader(d_file);
        if (reader.open()) {
            auto header = reader.read_header();
            // Symbol vector
            if (header.type == static_cast<uint8_t>(KType::SYMBOL)) {
                size_t count = (reader.file_size() - 8) / 4;  // 4 bytes per symbol index
                auto indices = reader.read_vector<int32_t>(count);

                // Column names are just file names in this case
                for (const auto& entry : std::filesystem::directory_iterator(table_path)) {
                    if (entry.is_regular_file() && entry.path().filename().string()[0] != '.') {
                        column_names.push_back(entry.path().filename().string());
                    }
                }
            }
            reader.close();
        }
    } else {
        // No .d file: column names = file names
        for (const auto& entry : std::filesystem::directory_iterator(table_path)) {
            if (entry.is_regular_file() && entry.path().filename().string()[0] != '.') {
                column_names.push_back(entry.path().filename().string());
            }
        }
    }

    // Read type from each column file
    for (const auto& col_name : column_names) {
        auto col_file = table_path / col_name;

        KDBFileReader reader(col_file);
        if (reader.open()) {
            auto header = reader.read_header();
            KType type = static_cast<KType>(header.type);

            size_t row_count = 0;
            // Estimate row count based on file size and type
            size_t data_size = reader.file_size() - 8;  // minus header
            size_t type_size = 0;

            switch (type) {
                case KType::BOOL:
                case KType::BYTE:
                case KType::CHAR:
                    type_size = 1; break;
                case KType::SHORT:
                    type_size = 2; break;
                case KType::INT:
                case KType::REAL:
                case KType::MONTH:
                case KType::DATE:
                case KType::MINUTE:
                case KType::SECOND:
                case KType::TIME:
                    type_size = 4; break;
                case KType::LONG:
                case KType::FLOAT:
                case KType::TIMESTAMP:
                case KType::TIMESPAN:
                case KType::DATETIME:
                    type_size = 8; break;
                case KType::GUID:
                    type_size = 16; break;
                case KType::SYMBOL:
                    type_size = 4; break;  // symbol index
            }

            if (type_size > 0) {
                row_count = data_size / type_size;
            }

            HDBColumn column(col_name, type, row_count);
            schema.push_back(column);

            reader.close();
        }
    }

    return !schema.empty();
}

std::vector<std::string> HDBLoader::load_sym_file(const std::filesystem::path& sym_path) {
    std::vector<std::string> symbols;

    std::ifstream file(sym_path, std::ios::binary);
    if (!file) return symbols;

    // sym file: null-terminated strings
    std::string current;
    char ch;

    while (file.get(ch)) {
        if (ch == '\0') {
            if (!current.empty()) {
                symbols.push_back(current);
                current.clear();
            }
        } else {
            current += ch;
        }
    }

    if (!current.empty()) {
        symbols.push_back(current);
    }

    return symbols;
}

std::vector<uint8_t> HDBLoader::load_column_data(const std::filesystem::path& column_file,
                                                  KType type,
                                                  size_t& row_count) {
    std::vector<uint8_t> data;

    KDBFileReader reader(column_file);
    if (!reader.open()) {
        return data;
    }

    auto header = reader.read_header();
    size_t data_size = reader.file_size() - 8;

    // Read raw data
    size_t elem_count = data_size / sizeof(uint8_t);
    data = reader.read_vector<uint8_t>(elem_count);

    reader.close();
    return data;
}

bool HDBLoader::load_table(const std::string& table_name,
                           const std::string& partition_date) {
    // Find table
    auto it = std::find_if(tables_.begin(), tables_.end(),
                          [&](const HDBTable& t) { return t.name == table_name; });

    if (it == tables_.end()) {
        std::cerr << "Table not found: " << table_name << std::endl;
        return false;
    }

    // Load partition(s)
    if (partition_date.empty()) {
        for (auto& partition : it->partitions) {
            std::cout << "Loading partition: " << partition.date << std::endl;
            for (const auto& col : partition.columns) {
                size_t row_count = 0;
                load_column_data(partition.path / col.name, col.type, row_count);
            }
        }
    } else {
        auto pit = std::find_if(it->partitions.begin(), it->partitions.end(),
                               [&](const HDBPartition& p) { return p.date == partition_date; });

        if (pit == it->partitions.end()) {
            std::cerr << "Partition not found: " << partition_date << std::endl;
            return false;
        }

        std::cout << "Loading partition: " << pit->date << std::endl;
        for (const auto& col : pit->columns) {
            size_t row_count = 0;
            load_column_data(pit->path / col.name, col.type, row_count);
        }
    }

    return true;
}

bool HDBLoader::export_to_zepto(const std::string& table_name,
                               const std::string& output_dir) {
    // Find table
    auto it = std::find_if(tables_.begin(), tables_.end(),
                          [&](const HDBTable& t) { return t.name == table_name; });

    if (it == tables_.end()) {
        return false;
    }

    ZeptoColumnWriter writer(output_dir, table_name);
    if (!writer.create_table(it->schema)) {
        return false;
    }

    // Export each partition
    for (const auto& partition : it->partitions) {
        std::cout << "Exporting partition: " << partition.date << std::endl;

        // Load and convert each column
        for (const auto& col : partition.columns) {
            auto col_file = partition.path / col.name;

            size_t row_count = 0;
            auto data = load_column_data(col_file, col.type, row_count);

            if (!data.empty()) {
                writer.write_column(col.name, data);
            }
        }
    }

    return writer.finalize();
}

std::string HDBLoader::zepto_type_from_ktype(KType ktype) {
    switch (ktype) {
        case KType::BOOL: return "BOOLEAN";
        case KType::BYTE: return "TINYINT";
        case KType::SHORT: return "SMALLINT";
        case KType::INT: return "INTEGER";
        case KType::LONG: return "BIGINT";
        case KType::REAL: return "REAL";
        case KType::FLOAT: return "DOUBLE";
        case KType::CHAR: return "CHAR";
        case KType::SYMBOL: return "VARCHAR";
        case KType::TIMESTAMP: return "TIMESTAMP";
        case KType::DATE: return "DATE";
        case KType::TIME: return "TIME";
        default: return "VARCHAR";
    }
}

// ============================================================================
// KDBFileReader Implementation
// ============================================================================

KDBFileReader::KDBFileReader(const std::filesystem::path& file_path)
    : file_path_(file_path)
    , fd_(-1)
    , mapped_data_(nullptr)
    , file_size_(0)
    , pos_(0)
    , is_little_endian_(true)
{
    // Detect system endianness
    uint16_t test = 1;
    is_little_endian_ = *reinterpret_cast<uint8_t*>(&test) == 1;
}

KDBFileReader::~KDBFileReader() {
    close();
}

bool KDBFileReader::open() {
    fd_ = ::open(file_path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    file_size_ = st.st_size;

    // Memory map the file
    mapped_data_ = static_cast<uint8_t*>(
        mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0)
    );

    if (mapped_data_ == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        mapped_data_ = nullptr;
        return false;
    }

    pos_ = 0;
    return true;
}

void KDBFileReader::close() {
    if (mapped_data_ && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, file_size_);
        mapped_data_ = nullptr;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

KDBFileReader::Header KDBFileReader::read_header() {
    Header header;

    if (file_size_ < 8) {
        header.byte_order = 1;
        header.type = 0;
        header.attr = 0;
        header.unused = 0;
        return header;
    }

    header.byte_order = mapped_data_[0];
    header.type = mapped_data_[1];
    header.attr = mapped_data_[2];
    header.unused = mapped_data_[3];

    pos_ = 8;  // Skip 8-byte header

    return header;
}

template<typename T>
std::vector<T> KDBFileReader::read_vector(size_t count) {
    std::vector<T> result;
    result.reserve(count);

    for (size_t i = 0; i < count && pos_ + sizeof(T) <= file_size_; ++i) {
        T value;
        std::memcpy(&value, mapped_data_ + pos_, sizeof(T));
        ensure_endianness(&value, sizeof(T));
        result.push_back(value);
        pos_ += sizeof(T);
    }

    return result;
}

// Explicit template instantiations
template std::vector<int8_t> KDBFileReader::read_vector<int8_t>(size_t);
template std::vector<int16_t> KDBFileReader::read_vector<int16_t>(size_t);
template std::vector<int32_t> KDBFileReader::read_vector<int32_t>(size_t);
template std::vector<int64_t> KDBFileReader::read_vector<int64_t>(size_t);
template std::vector<float> KDBFileReader::read_vector<float>(size_t);
template std::vector<double> KDBFileReader::read_vector<double>(size_t);

void KDBFileReader::ensure_endianness(void* data, size_t size) {
    // kdb+ uses little-endian by default
    // Only swap if system is big-endian
    if (!is_little_endian_) {
        uint8_t* bytes = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < size / 2; ++i) {
            std::swap(bytes[i], bytes[size - 1 - i]);
        }
    }
}

// ============================================================================
// ZeptoColumnWriter Implementation
// ============================================================================

ZeptoColumnWriter::ZeptoColumnWriter(const std::filesystem::path& output_dir,
                                   const std::string& table_name)
    : output_dir_(output_dir)
    , table_name_(table_name)
    , table_dir_(output_dir / table_name)
{}

bool ZeptoColumnWriter::create_table(const std::vector<HDBColumn>& schema) {
    // Create table directory
    std::filesystem::create_directories(table_dir_);

    // Write metadata
    return write_metadata(schema);
}

bool ZeptoColumnWriter::write_metadata(const std::vector<HDBColumn>& schema) {
    auto meta_file = table_dir_ / "metadata.json";
    std::ofstream out(meta_file);

    if (!out) return false;

    out << "{\n";
    out << "  \"table_name\": \"" << table_name_ << "\",\n";
    out << "  \"columns\": [\n";

    for (size_t i = 0; i < schema.size(); ++i) {
        const auto& col = schema[i];
        out << "    {\n";
        out << "      \"name\": \"" << col.name << "\",\n";
        out << "      \"type\": \"" << static_cast<int>(col.type) << "\",\n";
        out << "      \"row_count\": " << col.row_count << "\n";
        out << "    }";
        if (i < schema.size() - 1) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return true;
}

template<typename T>
bool ZeptoColumnWriter::write_column(const std::string& column_name,
                                    const std::vector<T>& data) {
    auto col_file = table_dir_ / (column_name + ".col");
    std::ofstream out(col_file, std::ios::binary);

    if (!out) return false;

    // Write column data
    out.write(reinterpret_cast<const char*>(data.data()),
              data.size() * sizeof(T));

    return out.good();
}

bool ZeptoColumnWriter::write_string_column(const std::string& column_name,
                                          const std::vector<std::string>& data) {
    auto col_file = table_dir_ / (column_name + ".col");
    std::ofstream out(col_file, std::ios::binary);

    if (!out) return false;

    // Write strings with length prefix
    for (const auto& str : data) {
        uint32_t len = str.length();
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        out.write(str.data(), len);
    }

    return out.good();
}

bool ZeptoColumnWriter::finalize() {
    // Write completion marker
    auto done_file = table_dir_ / ".done";
    std::ofstream out(done_file);
    return out.good();
}

} // namespace zeptodb::migration

// ============================================================================
// APEX-DB: HDB Loader Tests
// ============================================================================
#include "apex/migration/hdb_loader.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace apex::migration;
namespace fs = std::filesystem;

// ============================================================================
// Test Fixture
// ============================================================================

class HDBLoaderTest : public ::testing::Test {
protected:
    fs::path test_dir_;
    fs::path hdb_dir_;

    void SetUp() override {
        // Create temporary test directory
        test_dir_ = fs::temp_directory_path() / "apex_hdb_test";
        hdb_dir_ = test_dir_ / "hdb";
        fs::create_directories(hdb_dir_);
    }

    void TearDown() override {
        // Clean up
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    // Helper: Create mock sym file
    void create_sym_file(const std::vector<std::string>& symbols) {
        auto sym_path = hdb_dir_ / "sym";
        std::ofstream out(sym_path, std::ios::binary);

        for (const auto& sym : symbols) {
            out.write(sym.c_str(), sym.length());
            out.put('\0');
        }
    }

    // Helper: Create mock splayed table
    void create_splayed_table(const std::string& partition,
                             const std::string& table_name,
                             const std::vector<std::string>& columns) {
        auto table_dir = hdb_dir_ / partition / table_name;
        fs::create_directories(table_dir);

        // Create column files
        for (const auto& col : columns) {
            auto col_file = table_dir / col;
            std::ofstream out(col_file, std::ios::binary);

            // Write mock header (8 bytes)
            uint8_t header[8] = {1, 6, 0, 0, 0, 0, 0, 0};  // little-endian, type=INT
            out.write(reinterpret_cast<const char*>(header), 8);

            // Write mock data (10 integers)
            for (int i = 0; i < 10; ++i) {
                int32_t value = i * 100;
                out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            }
        }
    }

    // Helper: Create mock .d file (column names)
    void create_d_file(const std::string& partition,
                      const std::string& table_name,
                      const std::vector<std::string>& columns) {
        auto d_file = hdb_dir_ / partition / table_name / ".d";
        std::ofstream out(d_file, std::ios::binary);

        // Write mock header
        uint8_t header[8] = {1, 11, 0, 0, 0, 0, 0, 0};  // symbol vector
        out.write(reinterpret_cast<const char*>(header), 8);

        // Write symbol indices
        for (size_t i = 0; i < columns.size(); ++i) {
            int32_t idx = static_cast<int32_t>(i);
            out.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
        }
    }
};

// ============================================================================
// Sym File Tests
// ============================================================================

TEST_F(HDBLoaderTest, LoadSymFile) {
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT", "TSLA"};
    create_sym_file(symbols);

    HDBLoader loader(hdb_dir_);
    auto loaded_symbols = loader.load_sym_file(hdb_dir_ / "sym");

    EXPECT_EQ(loaded_symbols.size(), symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        EXPECT_EQ(loaded_symbols[i], symbols[i]);
    }
}

TEST_F(HDBLoaderTest, LoadSymFileEmpty) {
    create_sym_file({});

    HDBLoader loader(hdb_dir_);
    auto loaded_symbols = loader.load_sym_file(hdb_dir_ / "sym");

    EXPECT_EQ(loaded_symbols.size(), 0);
}

TEST_F(HDBLoaderTest, LoadSymFileNonExistent) {
    HDBLoader loader(hdb_dir_);
    auto loaded_symbols = loader.load_sym_file(hdb_dir_ / "nonexistent");

    EXPECT_EQ(loaded_symbols.size(), 0);
}

// ============================================================================
// HDB Structure Discovery Tests
// ============================================================================

TEST_F(HDBLoaderTest, DiscoverSingleTable) {
    create_splayed_table("2024.01.01", "trades", {"sym", "time", "price", "size"});

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    const auto& tables = loader.tables();
    EXPECT_EQ(tables.size(), 1);
    EXPECT_EQ(tables[0].name, "trades");
    EXPECT_TRUE(tables[0].is_partitioned);
    EXPECT_TRUE(tables[0].is_splayed);
}

TEST_F(HDBLoaderTest, DiscoverMultipleTables) {
    create_splayed_table("2024.01.01", "trades", {"sym", "price", "size"});
    create_splayed_table("2024.01.01", "quotes", {"sym", "bid", "ask"});

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    const auto& tables = loader.tables();
    EXPECT_EQ(tables.size(), 2);

    std::vector<std::string> table_names;
    for (const auto& table : tables) {
        table_names.push_back(table.name);
    }

    EXPECT_NE(std::find(table_names.begin(), table_names.end(), "trades"),
              table_names.end());
    EXPECT_NE(std::find(table_names.begin(), table_names.end(), "quotes"),
              table_names.end());
}

TEST_F(HDBLoaderTest, DiscoverMultiplePartitions) {
    create_splayed_table("2024.01.01", "trades", {"sym", "price"});
    create_splayed_table("2024.01.02", "trades", {"sym", "price"});
    create_splayed_table("2024.01.03", "trades", {"sym", "price"});

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    const auto& tables = loader.tables();
    EXPECT_EQ(tables.size(), 1);
    EXPECT_EQ(tables[0].name, "trades");
    EXPECT_EQ(tables[0].partitions.size(), 3);

    std::vector<std::string> partition_dates;
    for (const auto& partition : tables[0].partitions) {
        partition_dates.push_back(partition.date);
    }

    EXPECT_NE(std::find(partition_dates.begin(), partition_dates.end(), "2024.01.01"),
              partition_dates.end());
    EXPECT_NE(std::find(partition_dates.begin(), partition_dates.end(), "2024.01.02"),
              partition_dates.end());
    EXPECT_NE(std::find(partition_dates.begin(), partition_dates.end(), "2024.01.03"),
              partition_dates.end());
}

// ============================================================================
// Schema Reading Tests
// ============================================================================

TEST_F(HDBLoaderTest, ReadTableSchema) {
    std::vector<std::string> columns = {"sym", "time", "price", "size"};
    create_splayed_table("2024.01.01", "trades", columns);

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    const auto& tables = loader.tables();
    ASSERT_EQ(tables.size(), 1);

    const auto& schema = tables[0].schema;
    EXPECT_EQ(schema.size(), columns.size());

    for (const auto& col : schema) {
        EXPECT_NE(std::find(columns.begin(), columns.end(), col.name),
                  columns.end());
    }
}

// ============================================================================
// KDBFileReader Tests
// ============================================================================

TEST_F(HDBLoaderTest, ReadIntegerColumn) {
    // Create test column file
    auto col_file = test_dir_ / "test.col";
    std::ofstream out(col_file, std::ios::binary);

    // Header: little-endian, type=INT
    uint8_t header[8] = {1, 6, 0, 0, 0, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(header), 8);

    // Data: 5 integers
    std::vector<int32_t> data = {100, 200, 300, 400, 500};
    out.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(int32_t));
    out.close();

    // Read
    KDBFileReader reader(col_file);
    EXPECT_TRUE(reader.open());

    auto header_read = reader.read_header();
    EXPECT_EQ(header_read.byte_order, 1);  // little-endian
    EXPECT_EQ(header_read.type, 6);        // INT

    auto values = reader.read_vector<int32_t>(5);
    EXPECT_EQ(values.size(), 5);

    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], data[i]);
    }

    reader.close();
}

TEST_F(HDBLoaderTest, ReadFloatColumn) {
    // Create test column file
    auto col_file = test_dir_ / "test_float.col";
    std::ofstream out(col_file, std::ios::binary);

    // Header: little-endian, type=FLOAT
    uint8_t header[8] = {1, 9, 0, 0, 0, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(header), 8);

    // Data: 5 doubles
    std::vector<double> data = {100.5, 200.25, 300.125, 400.0625, 500.03125};
    out.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(double));
    out.close();

    // Read
    KDBFileReader reader(col_file);
    EXPECT_TRUE(reader.open());

    auto header_read = reader.read_header();
    EXPECT_EQ(header_read.type, 9);  // FLOAT (double)

    auto values = reader.read_vector<double>(5);
    EXPECT_EQ(values.size(), 5);

    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_DOUBLE_EQ(values[i], data[i]);
    }

    reader.close();
}

// ============================================================================
// Export Tests
// ============================================================================

TEST_F(HDBLoaderTest, ExportToAPEX) {
    create_splayed_table("2024.01.01", "trades", {"price", "size"});

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    auto output_dir = test_dir_ / "apex_output";
    EXPECT_TRUE(loader.export_to_apex("trades", output_dir.string()));

    // Verify output structure
    auto table_dir = output_dir / "trades";
    EXPECT_TRUE(fs::exists(table_dir));
    EXPECT_TRUE(fs::exists(table_dir / "metadata.json"));
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(HDBLoaderTest, ScanPerformance) {
    // Create 10 tables with 5 partitions each
    for (int d = 1; d <= 5; ++d) {
        std::string date = "2024.01.0" + std::to_string(d);
        for (int t = 0; t < 10; ++t) {
            std::string table = "table" + std::to_string(t);
            create_splayed_table(date, table, {"col1", "col2", "col3"});
        }
    }

    auto start = std::chrono::high_resolution_clock::now();

    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Should scan 10 tables × 5 partitions in < 1 second
    EXPECT_LT(duration.count(), 1000) << "Scan took " << duration.count() << "ms";

    std::cout << "HDB scan time (10 tables, 5 partitions): "
              << duration.count() << "ms" << std::endl;
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(HDBLoaderTest, NonExistentHDB) {
    auto bad_path = test_dir_ / "nonexistent";

    HDBLoader loader(bad_path);
    EXPECT_FALSE(loader.scan());
}

TEST_F(HDBLoaderTest, EmptyHDB) {
    HDBLoader loader(hdb_dir_);
    EXPECT_TRUE(loader.scan());

    const auto& tables = loader.tables();
    EXPECT_EQ(tables.size(), 0);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "zeptodb/storage/parquet_reader.h"
#include "zeptodb/ingestion/tick_plant.h"

#if ZEPTO_PARQUET_AVAILABLE
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#endif

namespace zeptodb::storage {

ParquetReadResult ParquetReader::read_file(const std::string& path,
                                            size_t limit) {
    ParquetReadResult result;

#if ZEPTO_PARQUET_AVAILABLE
    auto infile = arrow::io::ReadableFile::Open(path);
    if (!infile.ok()) {
        result.error = "Cannot open: " + path;
        return result;
    }

    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto st = parquet::arrow::OpenFile(*infile, arrow::default_memory_pool(), &reader);
    if (!st.ok()) {
        result.error = "Parquet open failed: " + st.ToString();
        return result;
    }

    std::shared_ptr<arrow::Table> table;
    st = reader->ReadTable(&table);
    if (!st.ok()) {
        result.error = "Parquet read failed: " + st.ToString();
        return result;
    }

    for (int i = 0; i < table->num_columns(); ++i)
        result.column_names.push_back(table->field(i)->name());

    size_t nrows = static_cast<size_t>(table->num_rows());
    if (limit > 0 && nrows > limit) nrows = limit;
    result.total_rows = nrows;

    for (size_t r = 0; r < nrows; ++r) {
        std::vector<int64_t> row;
        for (int c = 0; c < table->num_columns(); ++c) {
            auto chunk = table->column(c)->chunk(0);
            if (auto arr = std::dynamic_pointer_cast<arrow::Int64Array>(chunk))
                row.push_back(arr->Value(r));
            else
                row.push_back(0);
        }
        result.rows.push_back(std::move(row));
    }
#else
    result.error = "Parquet support not available (ZEPTO_PARQUET_AVAILABLE=0)";
    (void)path; (void)limit;
#endif

    return result;
}

size_t ParquetReader::ingest_file(const std::string& path,
                                   zeptodb::core::ZeptoPipeline& pipeline,
                                   size_t limit) {
    auto result = read_file(path, limit);
    if (!result.ok() || result.rows.empty()) return 0;

    // Find column indices
    int col_sym = -1, col_price = -1, col_vol = -1, col_ts = -1;
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (result.column_names[i] == "symbol")    col_sym   = static_cast<int>(i);
        if (result.column_names[i] == "price")     col_price = static_cast<int>(i);
        if (result.column_names[i] == "volume")    col_vol   = static_cast<int>(i);
        if (result.column_names[i] == "timestamp") col_ts    = static_cast<int>(i);
    }

    size_t count = 0;
    for (auto& row : result.rows) {
        zeptodb::ingestion::TickMessage msg{};
        if (col_sym >= 0)   msg.symbol_id = static_cast<uint32_t>(row[col_sym]);
        if (col_price >= 0) msg.price     = row[col_price];
        if (col_vol >= 0)   msg.volume    = row[col_vol];
        if (col_ts >= 0)    msg.recv_ts   = row[col_ts];
        if (pipeline.ingest_tick(msg)) ++count;
    }
    pipeline.drain_sync(result.rows.size() + 100);
    return count;
}

} // namespace zeptodb::storage

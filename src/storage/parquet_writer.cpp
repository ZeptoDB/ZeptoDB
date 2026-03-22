// ============================================================================
// Layer 1: HDB Parquet Writer — 구현
// ============================================================================

#include "apex/storage/parquet_writer.h"

#include <filesystem>
#include <fstream>

namespace apex::storage {

namespace fs = std::filesystem;

// ============================================================================
// 생성자
// ============================================================================
ParquetWriter::ParquetWriter(ParquetWriterConfig config)
    : config_(std::move(config))
{
#if APEX_PARQUET_AVAILABLE
    APEX_INFO("ParquetWriter 초기화: compression={}, row_group_size={}",
              static_cast<int>(config_.compression), config_.row_group_size);
#else
    APEX_WARN("ParquetWriter: libarrow/libparquet 없음 — Parquet 저장 비활성화");
    APEX_WARN("  설치: sudo dnf install -y arrow-devel parquet-devel");
#endif
}

// ============================================================================
// flush_to_file: Partition → 로컬 .parquet 파일
// ============================================================================
std::string ParquetWriter::flush_to_file(const Partition& partition,
                                          const std::string& output_dir)
{
    const auto& key = partition.key();

    if (partition.num_rows() == 0) {
        APEX_DEBUG("ParquetWriter: 빈 파티션 스킵 (symbol={}, hour={})",
                   key.symbol_id, key.hour_epoch);
        return "";
    }

#if APEX_PARQUET_AVAILABLE
    // 출력 디렉토리 생성
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
        APEX_WARN("ParquetWriter: 디렉토리 생성 실패: {} ({})", output_dir, ec.message());
        return "";
    }

    const std::string filename = parquet_filename(key.symbol_id, key.hour_epoch);
    const std::string filepath = output_dir + "/" + filename;

    // Arrow Table 빌드
    auto table = to_arrow_table(partition);
    if (!table) {
        APEX_WARN("ParquetWriter: Arrow Table 변환 실패 (symbol={}, hour={})",
                  key.symbol_id, key.hour_epoch);
        return "";
    }

    // 파일 출력 스트림
    auto result = arrow::io::FileOutputStream::Open(filepath);
    if (!result.ok()) {
        APEX_WARN("ParquetWriter: 파일 열기 실패: {} ({})",
                  filepath, result.status().message());
        return "";
    }
    auto outfile = result.ValueUnsafe();

    // Parquet 쓰기
    auto props = make_writer_props();
    auto status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                             outfile, config_.row_group_size, props);
    if (!status.ok()) {
        APEX_WARN("ParquetWriter: 쓰기 실패: {} ({})", filepath, status.message());
        return "";
    }

    // 파일 크기 추적
    const size_t file_size = static_cast<size_t>(fs::file_size(filepath, ec));
    files_written_.fetch_add(1, std::memory_order_relaxed);
    bytes_written_.fetch_add(file_size, std::memory_order_relaxed);

    APEX_INFO("Parquet 저장 완료: {} (rows={}, size={}B)",
              filepath, partition.num_rows(), file_size);
    return filepath;

#else
    APEX_WARN("ParquetWriter: libarrow 없음 — Parquet 저장 불가");
    return "";
#endif
}

#if APEX_PARQUET_AVAILABLE

// ============================================================================
// flush_to_buffer: Partition → Arrow Buffer (S3 직접 업로드용)
// ============================================================================
std::shared_ptr<arrow::Buffer> ParquetWriter::flush_to_buffer(const Partition& partition)
{
    if (partition.num_rows() == 0) return nullptr;

    auto table = to_arrow_table(partition);
    if (!table) return nullptr;

    // in-memory 버퍼 스트림
    auto buf_result = arrow::io::BufferOutputStream::Create();
    if (!buf_result.ok()) return nullptr;
    auto buf_stream = buf_result.ValueUnsafe();

    auto props = make_writer_props();
    auto status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                             buf_stream, config_.row_group_size, props);
    if (!status.ok()) {
        APEX_WARN("ParquetWriter::flush_to_buffer 실패: {}", status.message());
        return nullptr;
    }

    auto finish = buf_stream->Finish();
    if (!finish.ok()) return nullptr;

    bytes_written_.fetch_add(static_cast<size_t>(finish.ValueUnsafe()->size()),
                             std::memory_order_relaxed);
    return finish.ValueUnsafe();
}

// ============================================================================
// to_arrow_table: Partition → Arrow Table
// ============================================================================
std::shared_ptr<arrow::Table> ParquetWriter::to_arrow_table(const Partition& partition)
{
    arrow::SchemaBuilder schema_builder;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> arrays;

    for (const auto& col_ptr : partition.columns()) {
        if (!col_ptr) continue;

        auto arr = build_array(*col_ptr);
        if (!arr) continue;

        auto field = arrow::field(col_ptr->name(), arrow_type(col_ptr->type()));
        auto s = schema_builder.AddField(field);
        if (!s.ok()) continue;

        arrays.push_back(std::make_shared<arrow::ChunkedArray>(
            arrow::ArrayVector{arr}));
    }

    auto schema_result = schema_builder.Finish();
    if (!schema_result.ok()) return nullptr;

    return arrow::Table::Make(schema_result.ValueUnsafe(), arrays);
}

// ============================================================================
// build_array: ColumnVector → Arrow Array
// ============================================================================
std::shared_ptr<arrow::Array> ParquetWriter::build_array(const ColumnVector& col)
{
    const size_t n    = col.size();
    const void*  data = col.raw_data();
    if (n == 0 || !data) return nullptr;

    const size_t elem_sz = column_type_size(col.type());
    auto buf_result = arrow::Buffer::FromString(
        std::string(static_cast<const char*>(data), n * elem_sz));

    switch (col.type()) {
        case ColumnType::INT32: {
            arrow::Int32Builder builder;
            (void)builder.AppendValues(
                static_cast<const int32_t*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::INT64: {
            arrow::Int64Builder builder;
            (void)builder.AppendValues(
                static_cast<const int64_t*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::FLOAT32: {
            arrow::FloatBuilder builder;
            (void)builder.AppendValues(
                static_cast<const float*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::FLOAT64: {
            arrow::DoubleBuilder builder;
            (void)builder.AppendValues(
                static_cast<const double*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::TIMESTAMP_NS: {
            arrow::TimestampBuilder builder(
                arrow::timestamp(arrow::TimeUnit::NANO, "UTC"),
                arrow::default_memory_pool());
            (void)builder.AppendValues(
                static_cast<const int64_t*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::SYMBOL: {
            arrow::UInt32Builder builder;
            (void)builder.AppendValues(
                static_cast<const uint32_t*>(data), static_cast<int64_t>(n));
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
        case ColumnType::BOOL: {
            arrow::BooleanBuilder builder;
            const uint8_t* bdata = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < n; ++i) {
                (void)builder.Append(bdata[i] != 0);
            }
            std::shared_ptr<arrow::Array> arr;
            (void)builder.Finish(&arr);
            return arr;
        }
    }
    return nullptr;
}

// ============================================================================
// arrow_type: ColumnType → Arrow DataType
// ============================================================================
std::shared_ptr<arrow::DataType> ParquetWriter::arrow_type(ColumnType t)
{
    switch (t) {
        case ColumnType::INT32:        return arrow::int32();
        case ColumnType::INT64:        return arrow::int64();
        case ColumnType::FLOAT32:      return arrow::float32();
        case ColumnType::FLOAT64:      return arrow::float64();
        case ColumnType::TIMESTAMP_NS: return arrow::timestamp(arrow::TimeUnit::NANO, "UTC");
        case ColumnType::SYMBOL:       return arrow::uint32();
        case ColumnType::BOOL:         return arrow::boolean();
    }
    return arrow::null();
}

// ============================================================================
// make_writer_props: Parquet 쓰기 설정
// ============================================================================
std::shared_ptr<parquet::WriterProperties> ParquetWriter::make_writer_props() const
{
    auto builder = parquet::WriterProperties::Builder();

    switch (config_.compression) {
        case ParquetCompression::SNAPPY:
            builder.compression(parquet::Compression::SNAPPY);
            break;
        case ParquetCompression::ZSTD:
            builder.compression(parquet::Compression::ZSTD);
            break;
        case ParquetCompression::LZ4_RAW:
            builder.compression(parquet::Compression::LZ4_HADOOP);
            break;
        case ParquetCompression::NONE:
            builder.compression(parquet::Compression::UNCOMPRESSED);
            break;
    }

    if (config_.write_stats) {
        builder.enable_statistics();
    }

    return builder.build();
}

#endif // APEX_PARQUET_AVAILABLE

} // namespace apex::storage

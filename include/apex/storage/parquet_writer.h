#pragma once
// ============================================================================
// Layer 1: HDB Parquet Writer
// ============================================================================
// APEX-DB 파티션을 Apache Parquet 형식으로 직렬화.
//
// 설계 원칙:
//   - Arrow C++ API를 통해 ColumnVector → Arrow Array 변환 후 Parquet 저장
//   - 압축: SNAPPY(기본) / ZSTD / LZ4_RAW 선택 가능
//   - flush_to_file(): 로컬 .parquet 파일 생성
//   - flush_to_buffer(): S3/네트워크 업로드용 in-memory 직렬화
//
// 의존성: libarrow, libparquet (arrow-devel, parquet-devel)
//   Amazon Linux 2023: sudo dnf install -y arrow-devel parquet-devel
// ============================================================================

#include "apex/storage/column_store.h"
#include "apex/storage/partition_manager.h"
#include "apex/common/logger.h"

#include <cstdint>
#include <memory>
#include <string>

// Arrow/Parquet 가용성
#if __has_include(<arrow/api.h>) && __has_include(<parquet/arrow/writer.h>)
    #include <arrow/api.h>
    #include <arrow/io/api.h>
    #include <parquet/arrow/writer.h>
    #include <parquet/properties.h>
    #define APEX_PARQUET_AVAILABLE 1
#else
    #define APEX_PARQUET_AVAILABLE 0
#endif

namespace apex::storage {

// ============================================================================
// ParquetCompression: 지원 압축 코덱
// ============================================================================
enum class ParquetCompression : uint8_t {
    NONE    = 0,
    SNAPPY  = 1,   // 기본값 — 빠른 압축/해제
    ZSTD    = 2,   // 높은 압축률
    LZ4_RAW = 3,   // 최고속 압축 (LZ4)
};

// ============================================================================
// ParquetWriterConfig
// ============================================================================
struct ParquetWriterConfig {
    ParquetCompression compression    = ParquetCompression::SNAPPY;
    int64_t            row_group_size = 128LL * 1024 * 1024; // 128MB per row group
    bool               write_stats    = true;   // Parquet 통계 메타데이터 포함
};

// ============================================================================
// ParquetWriter: Partition → .parquet 직렬화
// ============================================================================
class ParquetWriter {
public:
    explicit ParquetWriter(ParquetWriterConfig config = {});

    // Non-copyable
    ParquetWriter(const ParquetWriter&) = delete;
    ParquetWriter& operator=(const ParquetWriter&) = delete;

    /// Partition → 로컬 .parquet 파일
    /// @param partition  봉인된 파티션
    /// @param output_dir 출력 디렉토리 (존재해야 함)
    /// @return 생성된 파일 경로 ("" on failure)
    std::string flush_to_file(const Partition& partition,
                              const std::string& output_dir);

#if APEX_PARQUET_AVAILABLE
    /// Partition → Arrow Buffer (S3 업로드 등 in-memory 전송용)
    /// @return nullptr on failure
    std::shared_ptr<arrow::Buffer> flush_to_buffer(const Partition& partition);

    /// ColumnVector 목록 → Arrow Table 변환 (내부/테스트용)
    static std::shared_ptr<arrow::Table> to_arrow_table(const Partition& partition);

private:
    /// Arrow Array 한 개 빌드
    static std::shared_ptr<arrow::Array> build_array(const ColumnVector& col);

    /// ColumnType → Arrow DataType 매핑
    static std::shared_ptr<arrow::DataType> arrow_type(ColumnType t);

    /// Parquet WriterProperties 구성
    std::shared_ptr<parquet::WriterProperties> make_writer_props() const;
#endif

private:
    ParquetWriterConfig config_;

    // 통계
    std::atomic<size_t> files_written_{0};
    std::atomic<size_t> bytes_written_{0};

public:
    [[nodiscard]] size_t files_written() const {
        return files_written_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t bytes_written() const {
        return bytes_written_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static bool parquet_available() {
        return APEX_PARQUET_AVAILABLE == 1;
    }
};

// ============================================================================
// 파일명 규칙: {symbol}_{hour_epoch}.parquet
// ============================================================================
inline std::string parquet_filename(SymbolId symbol, int64_t hour_epoch) {
    return std::to_string(symbol) + "_" + std::to_string(hour_epoch) + ".parquet";
}

} // namespace apex::storage

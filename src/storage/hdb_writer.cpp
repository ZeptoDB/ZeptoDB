// ============================================================================
// Layer 1: HDB Writer — 구현
// ============================================================================

#include "zeptodb/storage/hdb_writer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if ZEPTO_HDB_LZ4_AVAILABLE
    #include <lz4.h>
#endif

namespace zeptodb::storage {

namespace fs = std::filesystem;

// ============================================================================
// 생성자
// ============================================================================
HDBWriter::HDBWriter(const std::string& base_path, bool use_compression)
    : base_path_(base_path)
    , use_compression_(use_compression)
{
    ZEPTO_INFO("HDBWriter 초기화: base_path={}, compression={}",
              base_path_, use_compression_);

#if !ZEPTO_HDB_LZ4_AVAILABLE
    if (use_compression_) {
        ZEPTO_WARN("LZ4 라이브러리 없음 — 압축 비활성화 (패스스루 모드)");
        use_compression_ = false;
    }
#endif

    // 기본 디렉토리 생성
    if (!mkdir_p(base_path_)) {
        ZEPTO_WARN("HDBWriter: 기본 디렉토리 생성 실패: {}", base_path_);
    }
}

// ============================================================================
// flush_partition: 봉인된 파티션 → 디스크
// ============================================================================
size_t HDBWriter::flush_partition(const Partition& partition) {
    const auto& key = partition.key();

    // 파티션 상태 검증 (SEALED 또는 FLUSHING 이어야 함)
    const auto state = partition.state();
    if (state != Partition::State::SEALED &&
        state != Partition::State::FLUSHING) {
        ZEPTO_WARN("flush_partition: 파티션이 봉인 상태 아님 (symbol={}, hour={})",
                  key.symbol_id, key.hour_epoch);
        return 0;
    }

    const size_t num_rows = partition.num_rows();
    if (num_rows == 0) {
        ZEPTO_DEBUG("flush_partition: 빈 파티션 스킵 (symbol={}, hour={})",
                   key.symbol_id, key.hour_epoch);
        return 0;
    }

    // 파티션 디렉토리 생성 (table-scoped)
    const std::string dir = partition_dir(key.table_id, key.symbol_id, key.hour_epoch);
    if (!mkdir_p(dir)) {
        ZEPTO_WARN("flush_partition: 디렉토리 생성 실패: {}", dir);
        return 0;
    }

    size_t total_written = 0;

    // 컬럼별 직렬화
    for (const auto& col_ptr : partition.columns()) {
        if (!col_ptr) continue;
        const size_t written = write_column_file(dir, *col_ptr, key.table_id);
        total_written += written;
    }

    total_bytes_written_.fetch_add(total_written, std::memory_order_relaxed);
    partitions_flushed_.fetch_add(1, std::memory_order_relaxed);

    ZEPTO_INFO("HDB flush 완료: symbol={}, hour={}, rows={}, bytes={}",
              key.symbol_id, key.hour_epoch, num_rows, total_written);

    return total_written;
}

// ============================================================================
// snapshot_partition: 파티션 상태 무관 스냅샷 (ACTIVE 포함)
// ============================================================================
size_t HDBWriter::snapshot_partition(const Partition& partition,
                                      const std::string& snapshot_dir) {
    const auto& key = partition.key();

    const size_t num_rows = partition.num_rows();
    if (num_rows == 0) return 0;

    // 스냅샷 디렉토리: table-scoped
    //   table_id != 0 → {snapshot_dir}/t{table_id}/{symbol_id}/{hour_epoch}
    //   table_id == 0 → {snapshot_dir}/{symbol_id}/{hour_epoch}
    std::string dir;
    if (key.table_id != 0) {
        dir = snapshot_dir + "/t" + std::to_string(key.table_id) + "/" +
              std::to_string(key.symbol_id) + "/" +
              std::to_string(key.hour_epoch);
    } else {
        dir = snapshot_dir + "/" +
              std::to_string(key.symbol_id) + "/" +
              std::to_string(key.hour_epoch);
    }
    if (!mkdir_p(dir)) {
        ZEPTO_WARN("snapshot_partition: 디렉토리 생성 실패: {}", dir);
        return 0;
    }

    size_t total_written = 0;
    for (const auto& col_ptr : partition.columns()) {
        if (!col_ptr) continue;
        total_written += write_column_file(dir, *col_ptr, key.table_id);
    }

    ZEPTO_DEBUG("snapshot 완료: symbol={}, hour={}, rows={}, bytes={}",
               key.symbol_id, key.hour_epoch, num_rows, total_written);
    return total_written;
}

// ============================================================================
// write_column_file: 단일 컬럼 → 바이너리 파일
// ============================================================================
size_t HDBWriter::write_column_file(const std::string& dir_path,
                                     const ColumnVector& col,
                                     uint16_t table_id) {
    const std::string file_path = dir_path + "/" + col.name() + ".bin";

    // 원본 데이터
    const void*  src_data      = col.raw_data();
    const size_t elem_sz       = column_type_size(col.type());
    const size_t raw_data_size = col.size() * elem_sz;

    if (raw_data_size == 0) {
        ZEPTO_DEBUG("컬럼 '{}'가 비어있음 — 스킵", col.name());
        return 0;
    }

    // --- 압축 처리 ---
    std::vector<char> compressed_buf;
    const void*  write_data   = src_data;
    size_t       write_size   = raw_data_size;
    HDBCompression compression = HDBCompression::NONE;

#if ZEPTO_HDB_LZ4_AVAILABLE
    if (use_compression_) {
        const int max_dst = LZ4_compressBound(static_cast<int>(raw_data_size));
        compressed_buf.resize(static_cast<size_t>(max_dst));

        const int compressed_size = LZ4_compress_default(
            static_cast<const char*>(src_data),
            compressed_buf.data(),
            static_cast<int>(raw_data_size),
            max_dst
        );

        if (compressed_size > 0 &&
            static_cast<size_t>(compressed_size) < raw_data_size) {
            // 압축이 효과가 있는 경우에만 사용
            write_data   = compressed_buf.data();
            write_size   = static_cast<size_t>(compressed_size);
            compression  = HDBCompression::LZ4;
        }
        // 압축 효과 없으면 raw 데이터 그대로 사용
    }
#endif

    // --- 헤더 구성 (v2: includes table_id) ---
    HDBFileHeader header{};
    std::memcpy(header.magic, HDB_MAGIC, 5);
    header.version           = HDB_VERSION;   // = 2
    header.col_type          = static_cast<uint8_t>(col.type());
    header.compression       = static_cast<uint8_t>(compression);
    header.row_count         = static_cast<uint64_t>(col.size());
    header.data_size         = static_cast<uint64_t>(write_size);
    header.uncompressed_size = static_cast<uint64_t>(raw_data_size);
    header.table_id          = table_id;
    // header.reserved is zero-initialized by {}.

    // --- 파일 쓰기 ---
    const int fd = ::open(file_path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd < 0) {
        ZEPTO_WARN("컬럼 파일 열기 실패: {} (errno={})", file_path, errno);
        return 0;
    }

    // 헤더 쓰기
    ssize_t wrote = ::write(fd, &header, sizeof(header));
    if (wrote != static_cast<ssize_t>(sizeof(header))) {
        ZEPTO_WARN("헤더 쓰기 실패: {} (wrote={})", file_path, wrote);
        ::close(fd);
        return 0;
    }

    // 데이터 쓰기 (분할 쓰기 처리)
    const char* data_ptr    = static_cast<const char*>(write_data);
    size_t      remaining   = write_size;
    size_t      data_written = 0;

    while (remaining > 0) {
        const ssize_t n = ::write(fd, data_ptr + data_written, remaining);
        if (n <= 0) {
            ZEPTO_WARN("데이터 쓰기 실패: {} (errno={})", file_path, errno);
            ::close(fd);
            return sizeof(header) + data_written;
        }
        data_written += static_cast<size_t>(n);
        remaining    -= static_cast<size_t>(n);
    }

    ::close(fd);

    const size_t total = sizeof(header) + write_size;
    ZEPTO_DEBUG("컬럼 기록: {} (rows={}, raw={}B, write={}B, comp={})",
               col.name(), col.size(), raw_data_size, write_size,
               compression == HDBCompression::LZ4 ? "LZ4" : "NONE");

    return total;
}

// ============================================================================
// partition_dir: 파티션 디렉토리 경로
// ============================================================================
std::string HDBWriter::partition_dir(uint16_t table_id, SymbolId symbol, int64_t hour_epoch) const {
    // table_id != 0 → {base}/t{table_id}/{symbol}/{hour}
    // table_id == 0 → {base}/{symbol}/{hour}   (legacy, pre-v2 layout)
    if (table_id != 0) {
        return base_path_ + "/t" + std::to_string(table_id) + "/" +
               std::to_string(symbol) + "/" +
               std::to_string(hour_epoch);
    }
    return base_path_ + "/" +
           std::to_string(symbol) + "/" +
           std::to_string(hour_epoch);
}

// ============================================================================
// mkdir_p: 재귀 디렉토리 생성
// ============================================================================
bool HDBWriter::mkdir_p(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        ZEPTO_WARN("mkdir_p 실패: {} ({})", path, ec.message());
        return false;
    }
    return true;
}

} // namespace zeptodb::storage

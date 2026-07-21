// ============================================================================
// Layer 1: HDB Writer — 구현
// ============================================================================

#include "zeptodb/storage/hdb_writer.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
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

namespace {

std::atomic<uint64_t> g_column_temp_sequence{0};

bool write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<size_t>(result);
    }
    return true;
}

bool valid_column_type(ColumnType type) {
    return column_type_size(type) != 0;
}

}  // namespace

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

    const auto partition_lock = partition.lock_for_write();
    const size_t num_rows = partition.num_rows();
    for (const auto& col_ptr : partition.columns()) {
        if (!col_ptr || !valid_column_type(col_ptr->type()) ||
            col_ptr->size() != num_rows) {
            ZEPTO_WARN(
                "flush_partition: inconsistent column rows/types "
                "(table={}, symbol={}, hour={})",
                key.table_id, key.symbol_id, key.hour_epoch);
            return 0;
        }
    }
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

    // 컬럼별 직렬화
    size_t total_written = 0;
    for (const auto& col_ptr : partition.columns()) {
        const size_t written = write_column_file(dir, *col_ptr, key.table_id);
        if (written == 0) {
            ZEPTO_WARN(
                "flush_partition: column write failed "
                "(table={}, symbol={}, hour={}, column={})",
                key.table_id, key.symbol_id, key.hour_epoch, col_ptr->name());
            return 0;
        }
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
    const auto result = snapshot_partition_checked(partition, snapshot_dir);
    return result.ok ? result.bytes_written : 0;
}

HDBSnapshotWriteResult HDBWriter::snapshot_partition_checked(
    const Partition& partition,
    const std::string& snapshot_dir) {
    const auto& key = partition.key();
    const auto partition_lock = partition.lock_for_write();

    const size_t num_rows = partition.num_rows();
    for (const auto& col_ptr : partition.columns()) {
        if (!col_ptr || !valid_column_type(col_ptr->type()) ||
            col_ptr->size() != num_rows) {
            ZEPTO_WARN(
                "snapshot_partition: inconsistent column rows/types "
                "(table={}, symbol={}, hour={})",
                key.table_id, key.symbol_id, key.hour_epoch);
            return {};
        }
        if (col_ptr->type() == ColumnType::STRING ||
            col_ptr->type() == ColumnType::SYMBOL) {
            ZEPTO_WARN(
                "snapshot_partition: dictionary column is not recoverable "
                "without durable dictionary metadata (table={}, column={})",
                key.table_id, col_ptr->name());
            return {};
        }
    }
    if (num_rows == 0) {
        return {.ok = true, .bytes_written = 0, .rows_written = 0};
    }

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
        return {};
    }

    size_t total_written = 0;
    for (const auto& col_ptr : partition.columns()) {
        const size_t written = write_column_file(dir, *col_ptr, key.table_id);
        if (written == 0) {
            ZEPTO_WARN(
                "snapshot_partition: column write failed "
                "(table={}, symbol={}, hour={}, column={})",
                key.table_id, key.symbol_id, key.hour_epoch, col_ptr->name());
            return {};
        }
        total_written += written;
    }

    ZEPTO_DEBUG("snapshot 완료: symbol={}, hour={}, rows={}, bytes={}",
               key.symbol_id, key.hour_epoch, num_rows, total_written);
    return {
        .ok = true,
        .bytes_written = total_written,
        .rows_written = num_rows,
    };
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
    if (elem_sz == 0 || col.size() > std::numeric_limits<size_t>::max() / elem_sz) {
        ZEPTO_WARN("invalid column dimensions: {}", col.name());
        return 0;
    }
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
    if (use_compression_ &&
        raw_data_size <= static_cast<size_t>(std::numeric_limits<int>::max())) {
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

    // Publish each column with rename so a failed write never truncates the
    // last readable file in place. Whole-snapshot atomicity is provided by the
    // generation manifest in FlushManager.
    const uint64_t sequence = g_column_temp_sequence.fetch_add(
        1, std::memory_order_relaxed);
    const std::string temp_path = file_path + ".tmp." +
        std::to_string(static_cast<uint64_t>(::getpid())) + "." +
        std::to_string(sequence);
    const int fd = ::open(temp_path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          0644);
    if (fd < 0) {
        ZEPTO_WARN("failed to open temporary column file: {} (errno={})",
                   temp_path, errno);
        return 0;
    }

    if (!write_all(fd, &header, sizeof(header)) ||
        !write_all(fd, write_data, write_size)) {
        const int saved_errno = errno;
        ::close(fd);
        ::unlink(temp_path.c_str());
        ZEPTO_WARN("failed to write column file: {} (errno={})",
                   temp_path, saved_errno);
        return 0;
    }

    if (::close(fd) != 0) {
        const int saved_errno = errno;
        ::unlink(temp_path.c_str());
        ZEPTO_WARN("failed to close temporary column file: {} (errno={})",
                   temp_path, saved_errno);
        return 0;
    }

    std::error_code rename_error;
    fs::rename(temp_path, file_path, rename_error);
    if (rename_error) {
        ::unlink(temp_path.c_str());
        ZEPTO_WARN("failed to publish column file: {} -> {} ({})",
                  temp_path, file_path, rename_error.message());
        return 0;
    }

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

// ============================================================================
// Layer 1: HDB Reader — 구현
// ============================================================================

#include "zeptodb/storage/hdb_reader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if ZEPTO_HDB_LZ4_AVAILABLE
    #include <lz4.h>
#endif

namespace zeptodb::storage {

namespace fs = std::filesystem;

// ============================================================================
// 생성자
// ============================================================================
HDBReader::HDBReader(const std::string& base_path)
    : base_path_(base_path)
{
    ZEPTO_INFO("HDBReader 초기화: base_path={}", base_path_);
}

// ============================================================================
// read_column: 컬럼 파일 mmap 읽기
// ============================================================================
MappedColumn HDBReader::read_column(SymbolId symbol,
                                     int64_t  hour_epoch,
                                     const std::string& col_name) {
    MappedColumn result;

    const std::string file_path = column_file_path(symbol, hour_epoch, col_name);

    // 파일 열기
    const int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ZEPTO_DEBUG("HDB 컬럼 파일 없음: {}", file_path);
        return result;
    }

    // 파일 크기 확인
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ZEPTO_WARN("fstat 실패: {} (errno={})", file_path, errno);
        ::close(fd);
        return result;
    }

    const size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size < sizeof(HDBFileHeader)) {
        ZEPTO_WARN("파일이 너무 작음: {} ({} bytes)", file_path, file_size);
        ::close(fd);
        return result;
    }

    // mmap 전체 파일
    void* mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        ZEPTO_WARN("mmap 실패: {} (errno={})", file_path, errno);
        ::close(fd);
        return result;
    }

    // madvise: 순차 접근 힌트 → 커널 프리패치 최적화
    ::madvise(mapped, file_size, MADV_SEQUENTIAL);

    // 헤더 파싱
    const auto* header = static_cast<const HDBFileHeader*>(mapped);

    // 매직 바이트 검증
    if (std::memcmp(header->magic, HDB_MAGIC, 5) != 0) {
        ZEPTO_WARN("HDB 매직 바이트 불일치: {}", file_path);
        ::munmap(mapped, file_size);
        ::close(fd);
        return result;
    }

    // 버전 확인
    if (header->version != HDB_VERSION) {
        ZEPTO_WARN("HDB 버전 불일치: file={}, expect={}", header->version, HDB_VERSION);
        // 하위 호환성을 위해 경고만 출력하고 계속 진행
    }

    const ColumnType col_type   = static_cast<ColumnType>(header->col_type);
    const size_t     row_count  = static_cast<size_t>(header->row_count);
    const HDBCompression comp   = static_cast<HDBCompression>(header->compression);
    const size_t data_size      = static_cast<size_t>(header->data_size);
    const size_t uncomp_size    = static_cast<size_t>(header->uncompressed_size);

    // 데이터 포인터 (헤더 이후)
    const char* data_start = static_cast<const char*>(mapped) + sizeof(HDBFileHeader);

    if (comp == HDBCompression::NONE) {
        // 압축 없음 → mmap 포인터 직접 반환 (Zero-copy)
        result.data        = data_start;
        result.num_rows    = row_count;
        result.type        = col_type;
        result.fd          = fd;
        result.mapped_size = file_size;
        result.mapped_base = mapped;

    } else if (comp == HDBCompression::LZ4) {
        // LZ4 압축 해제 → 버퍼에 복사 (Zero-copy 불가, 복사 필요)
#if ZEPTO_HDB_LZ4_AVAILABLE
        result.decompressed_buf.resize(uncomp_size);

        const int decomp_result = LZ4_decompress_safe(
            data_start,
            result.decompressed_buf.data(),
            static_cast<int>(data_size),
            static_cast<int>(uncomp_size)
        );

        if (decomp_result < 0 || static_cast<size_t>(decomp_result) != uncomp_size) {
            ZEPTO_WARN("LZ4 압축 해제 실패: {} (result={})", file_path, decomp_result);
            ::munmap(mapped, file_size);
            ::close(fd);
            return result;
        }

        result.data        = result.decompressed_buf.data();
        result.num_rows    = row_count;
        result.type        = col_type;
        result.fd          = fd;
        result.mapped_size = file_size;
        result.mapped_base = mapped;
#else
        ZEPTO_WARN("LZ4 압축 파일이지만 LZ4 라이브러리 없음: {}", file_path);
        ::munmap(mapped, file_size);
        ::close(fd);
        return result;
#endif
    } else {
        ZEPTO_WARN("알 수 없는 압축 타입: {}", static_cast<int>(comp));
        ::munmap(mapped, file_size);
        ::close(fd);
        return result;
    }

    ZEPTO_DEBUG("HDB 컬럼 읽기 성공: {} (rows={}, comp={})",
               col_name, row_count,
               comp == HDBCompression::LZ4 ? "LZ4" : "NONE");

    return result;
}

// ============================================================================
// list_partitions: 심볼의 파티션 목록
// ============================================================================
std::vector<int64_t> HDBReader::list_partitions(SymbolId symbol) const {
    std::vector<int64_t> result;

    const std::string sym_dir = base_path_ + "/" + std::to_string(symbol);

    std::error_code ec;
    if (!fs::is_directory(sym_dir, ec)) {
        return result;
    }

    for (const auto& entry : fs::directory_iterator(sym_dir, ec)) {
        if (!entry.is_directory()) continue;

        // 디렉토리 이름 = hour_epoch
        try {
            const int64_t hour = std::stoll(entry.path().filename().string());
            result.push_back(hour);
        } catch (...) {
            // 숫자가 아닌 디렉토리 무시
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

// ============================================================================
// list_partitions_in_range: 시간 범위의 파티션 목록
// ============================================================================
std::vector<int64_t> HDBReader::list_partitions_in_range(
    SymbolId symbol, int64_t from_ns, int64_t to_ns) const
{
    constexpr int64_t NS_PER_HOUR = 3600LL * 1'000'000'000LL;

    // hour_epoch 경계로 정렬
    const int64_t from_hour = (from_ns / NS_PER_HOUR) * NS_PER_HOUR;
    const int64_t to_hour   = (to_ns   / NS_PER_HOUR) * NS_PER_HOUR;

    auto all = list_partitions(symbol);
    std::vector<int64_t> result;
    result.reserve(all.size());

    for (const int64_t h : all) {
        if (h >= from_hour && h <= to_hour) {
            result.push_back(h);
        }
    }

    return result;
}

// ============================================================================
// column_file_path: 컬럼 파일 경로
// ============================================================================
std::string HDBReader::column_file_path(SymbolId symbol,
                                          int64_t  hour_epoch,
                                          const std::string& col_name) const {
    return base_path_ + "/" +
           std::to_string(symbol) + "/" +
           std::to_string(hour_epoch) + "/" +
           col_name + ".bin";
}

} // namespace zeptodb::storage

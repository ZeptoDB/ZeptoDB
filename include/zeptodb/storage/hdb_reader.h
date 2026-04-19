#pragma once
// ============================================================================
// Layer 1: HDB Reader — 메모리맵 기반 컬럼 읽기 엔진
// ============================================================================
// 설계 원칙:
//   - mmap으로 컬럼 파일을 메모리에 매핑 (Zero-copy 읽기)
//   - 실행 엔진(ColumnVector)과 호환되는 포인터 반환
//   - MappedColumn: RAII 자동 munmap (소멸자에서 정리)
//   - 시간 범위에 걸친 다중 파티션 조회 지원
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/common/logger.h"
#include "zeptodb/storage/column_store.h"
#include "zeptodb/storage/hdb_writer.h"

#include <string>
#include <vector>
#include <cstdint>

// mmap 헤더
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace zeptodb::storage {

// ============================================================================
// MappedColumn: mmap된 컬럼 뷰 (RAII)
// ============================================================================
struct MappedColumn {
    const void*  data         = nullptr;  // mmap 포인터 (헤더 이후 데이터 시작)
    size_t       num_rows     = 0;
    ColumnType   type         = ColumnType::INT64;
    int          fd           = -1;       // 파일 디스크립터 (정리용)
    size_t       mapped_size  = 0;        // mmap된 전체 크기 (헤더 포함)
    void*        mapped_base  = nullptr;  // mmap 반환 포인터 (munmap에 사용)

    // 압축 해제 버퍼 (LZ4 압축인 경우)
    std::vector<char> decompressed_buf;

    // Valid 여부
    [[nodiscard]] bool valid() const { return data != nullptr && num_rows > 0; }

    // 타입안전 span 접근
    template <typename T>
    [[nodiscard]] std::span<const T> as_span() const {
        return std::span<const T>(static_cast<const T*>(data), num_rows);
    }

    // RAII 소멸자: mmap/fd 자동 해제
    ~MappedColumn() {
        if (mapped_base && mapped_base != MAP_FAILED && mapped_size > 0) {
            ::munmap(mapped_base, mapped_size);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }

    // Move-only (파일 핸들 소유)
    MappedColumn() = default;
    MappedColumn(const MappedColumn&) = delete;
    MappedColumn& operator=(const MappedColumn&) = delete;

    MappedColumn(MappedColumn&& other) noexcept
        : data(other.data)
        , num_rows(other.num_rows)
        , type(other.type)
        , fd(other.fd)
        , mapped_size(other.mapped_size)
        , mapped_base(other.mapped_base)
        , decompressed_buf(std::move(other.decompressed_buf))
    {
        other.data        = nullptr;
        other.fd          = -1;
        other.mapped_base = nullptr;
        other.mapped_size = 0;
    }

    MappedColumn& operator=(MappedColumn&& other) noexcept {
        if (this != &other) {
            // 기존 리소스 정리
            if (mapped_base && mapped_base != MAP_FAILED && mapped_size > 0) {
                ::munmap(mapped_base, mapped_size);
            }
            if (fd >= 0) {
                ::close(fd);
            }

            data        = other.data;
            num_rows    = other.num_rows;
            type        = other.type;
            fd          = other.fd;
            mapped_size = other.mapped_size;
            mapped_base = other.mapped_base;
            decompressed_buf = std::move(other.decompressed_buf);

            other.data        = nullptr;
            other.fd          = -1;
            other.mapped_base = nullptr;
            other.mapped_size = 0;
        }
        return *this;
    }
};

// ============================================================================
// HDBReader: HDB 디스크 데이터 읽기 (mmap 기반)
// ============================================================================
class HDBReader {
public:
    /// @param base_path  HDB 루트 디렉토리 (HDBWriter와 동일)
    explicit HDBReader(const std::string& base_path);

    // Non-copyable
    HDBReader(const HDBReader&) = delete;
    HDBReader& operator=(const HDBReader&) = delete;

    /// 특정 파티션의 컬럼을 mmap으로 읽기
    /// @param table_id    table_id (0 = legacy layout)
    /// @param symbol      심볼 ID
    /// @param hour_epoch  시간 파티션 epoch (나노초)
    /// @param col_name    컬럼 이름 (예: "price", "volume")
    /// @return MappedColumn (valid() == false 이면 읽기 실패)
    MappedColumn read_column(uint16_t table_id,
                              SymbolId symbol,
                              int64_t  hour_epoch,
                              const std::string& col_name);

    /// Legacy overload (table_id = 0).
    MappedColumn read_column(SymbolId symbol,
                              int64_t  hour_epoch,
                              const std::string& col_name) {
        return read_column(0, symbol, hour_epoch, col_name);
    }

    /// 특정 심볼의 사용 가능한 파티션 목록 반환 (hour_epoch 오름차순)
    std::vector<int64_t> list_partitions(uint16_t table_id, SymbolId symbol) const;
    std::vector<int64_t> list_partitions(SymbolId symbol) const {
        return list_partitions(0, symbol);
    }

    /// 시간 범위에 속하는 파티션 목록 반환
    std::vector<int64_t> list_partitions_in_range(uint16_t table_id,
                                                   SymbolId symbol,
                                                   int64_t  from_ns,
                                                   int64_t  to_ns) const;
    std::vector<int64_t> list_partitions_in_range(SymbolId symbol,
                                                   int64_t  from_ns,
                                                   int64_t  to_ns) const {
        return list_partitions_in_range(0, symbol, from_ns, to_ns);
    }

    [[nodiscard]] const std::string& base_path() const { return base_path_; }

private:
    std::string column_file_path(uint16_t table_id,
                                  SymbolId symbol,
                                  int64_t  hour_epoch,
                                  const std::string& col_name) const;

    std::string base_path_;
};

} // namespace zeptodb::storage

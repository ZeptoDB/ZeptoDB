#pragma once
// ============================================================================
// Layer 1: HDB Writer — 컬럼형 바이너리 파일 직렬화 엔진
// ============================================================================
// 설계 원칙:
//   - 봉인(SEALED)된 Partition을 디스크에 컬럼별 바이너리 파일로 저장
//   - 디렉토리 구조: {base_path}/{symbol_id}/{hour_epoch}/{col_name}.bin
//   - 파일 헤더: magic, version, type, compression flag, row_count, data_size
//   - LZ4 블록 압축 지원 (선택적 컴파일 옵션)
//   - 인제스션 핫패스 비차단: 별도 스레드에서 호출됨
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/common/logger.h"
#include "zeptodb/storage/column_store.h"
#include "zeptodb/storage/partition_manager.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// LZ4 가용성 확인: lz4-devel 설치 시 활성화
#if __has_include(<lz4.h>)
    #include <lz4.h>
    #define ZEPTO_HDB_LZ4_AVAILABLE 1
#else
    #define ZEPTO_HDB_LZ4_AVAILABLE 0
#endif

namespace zeptodb::storage {

// ============================================================================
// HDB File Header
// ----------------------------------------------------------------------------
// v1 (32 bytes, legacy): no table_id field; files assumed table_id = 0.
// v2 (40 bytes): appends uint16_t table_id + 6 reserved bytes for alignment.
// Readers MUST accept both: dispatch on `version` byte at offset 5.
// File layout: [Header] [data bytes (compressed or raw)]
// ============================================================================
struct HDBFileHeader {
    uint8_t  magic[5];           // "APEXH" (0x41 0x50 0x45 0x58 0x48)
    uint8_t  version;            // 1 = legacy 32B, 2 = 40B with table_id
    uint8_t  col_type;           // ColumnType (uint8_t cast)
    uint8_t  compression;        // 0=None, 1=LZ4 block
    uint64_t row_count;          // number of rows
    uint64_t data_size;          // payload bytes after header (compressed if LZ4)
    uint64_t uncompressed_size;  // original raw size (== data_size if uncompressed)
    // --- v2 fields (below) ---
    uint16_t table_id;           // SchemaRegistry-assigned id; 0 = legacy/default
    uint8_t  reserved[6];        // padding for 8-byte alignment / future use
    // Total v2: 5+1+1+1+8+8+8+2+6 = 40 bytes
};
static_assert(sizeof(HDBFileHeader) == 40, "HDBFileHeader v2 must be 40 bytes");

// v1 header layout size for backward-compatible reads.
inline constexpr size_t HDB_HEADER_V1_SIZE = 32;
inline constexpr size_t HDB_HEADER_V2_SIZE = 40;

// Magic bytes
inline constexpr uint8_t HDB_MAGIC[5] = {'A', 'P', 'E', 'X', 'H'};
inline constexpr uint8_t HDB_VERSION   = 2;  // current writer version

// 압축 플래그
enum class HDBCompression : uint8_t {
    NONE = 0,
    LZ4  = 1,
};

// ============================================================================
// HDBWriter: Partition → 디스크 직렬화
// ============================================================================
class HDBWriter {
public:
    /// @param base_path  HDB 루트 디렉토리 (예: "/data/hdb")
    /// @param use_compression  LZ4 압축 사용 여부 (lz4 가용 시에만 실제 압축)
    explicit HDBWriter(const std::string& base_path, bool use_compression = true);

    // Non-copyable (통계 카운터 atomic 멤버)
    HDBWriter(const HDBWriter&) = delete;
    HDBWriter& operator=(const HDBWriter&) = delete;

    /// 봉인된 파티션을 디스크에 기록
    /// @return 기록된 바이트 수 (모든 컬럼 합산, 헤더 포함)
    /// @note  디스크 오류 시 경고 로그 후 0 반환 (예외 없음)
    size_t flush_partition(const Partition& partition);

    /// 파티션 상태(ACTIVE/SEALED 무관)를 snapshot_dir에 스냅샷
    /// 기존 파일 덮어쓰기 — 장중 크래시 복구용
    /// @return 기록된 바이트 수
    size_t snapshot_partition(const Partition& partition,
                              const std::string& snapshot_dir);

    // --- 통계 ---
    [[nodiscard]] size_t total_bytes_written() const {
        return total_bytes_written_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t partitions_flushed() const {
        return partitions_flushed_.load(std::memory_order_relaxed);
    }

    /// 압축 지원 여부 (lz4-devel 없으면 false)
    [[nodiscard]] static bool lz4_available() {
#if ZEPTO_HDB_LZ4_AVAILABLE
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] const std::string& base_path() const { return base_path_; }

private:
    /// 단일 ColumnVector를 파일로 기록
    /// @param table_id  파일 헤더에 기록할 table_id (0 = legacy)
    /// @return 기록된 바이트 (헤더 포함)
    size_t write_column_file(const std::string& dir_path,
                              const ColumnVector& col,
                              uint16_t table_id = 0);

    /// 파티션 디렉토리 경로 계산 (table-scoped)
    /// table_id != 0 → {base_path}/t{table_id}/{symbol_id}/{hour_epoch}
    /// table_id == 0 → {base_path}/{symbol_id}/{hour_epoch}  (legacy layout)
    std::string partition_dir(uint16_t table_id, SymbolId symbol, int64_t hour_epoch) const;

    /// Legacy overload (table_id = 0). Preserved for callers that don't know
    /// about table_id yet (recovery, older tests).
    std::string partition_dir(SymbolId symbol, int64_t hour_epoch) const {
        return partition_dir(0, symbol, hour_epoch);
    }

    /// 디렉토리 재귀 생성
    static bool mkdir_p(const std::string& path);

    std::string          base_path_;
    bool                 use_compression_;

    std::atomic<size_t>  total_bytes_written_{0};
    std::atomic<size_t>  partitions_flushed_{0};
};

} // namespace zeptodb::storage

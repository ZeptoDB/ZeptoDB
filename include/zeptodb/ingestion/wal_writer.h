#pragma once
// ============================================================================
// Layer 2: WAL Writer — Write-Ahead Log for Crash Recovery
// ============================================================================
// 문서 근거: layer2_ingestion_network.md §4
//   - 데이터 수신 즉시 WAL에 순차 기록 (영구성 확보)
//   - 비동기 flush로 수집 속도에 영향 없음
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"
#include <string>
#include <fstream>
#include <mutex>

namespace zeptodb::ingestion {

class WALWriter {
public:
    explicit WALWriter(const std::string& path);
    ~WALWriter();

    /// 틱 메시지를 WAL에 기록 (바이너리 append)
    bool write(const TickMessage& msg);

    /// 디스크에 강제 flush
    void flush();

    /// 기록된 총 메시지 수
    [[nodiscard]] size_t message_count() const { return count_; }

private:
    std::ofstream file_;
    size_t count_ = 0;
    std::mutex mutex_;  // WAL은 순차 기록이므로 최소한의 동기화
};

} // namespace zeptodb::ingestion

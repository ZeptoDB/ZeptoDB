#pragma once
// ============================================================================
// Layer 2: Tick Plant — Market Data Ingestion Router
// ============================================================================
// 문서 근거: layer2_ingestion_network.md §1, §3
//   - 수신 → WAL 기록 → Ring Buffer 분배 → RDB/CEP 구독
//   - 순서 보장 (FIFO + Global Timestamp)
// ============================================================================

#include "zeptodb/common/types.h"
#include "zeptodb/ingestion/ring_buffer.h"
#include <cstdint>
#include <chrono>

namespace zeptodb::ingestion {

// ============================================================================
// TickMessage: 수신 틱 데이터 표준 구조체
// ============================================================================
struct APEX_CACHE_ALIGNED TickMessage {
    SeqNum    seq_num;      // 글로벌 순번
    Timestamp recv_ts;      // 수신 타임스탬프 (nanosecond)
    SymbolId  symbol_id;    // 종목 ID
    Price     price;        // 체결가 (int64 or bit_cast'd double)
    Volume    volume;       // 체결량
    uint8_t   msg_type;     // 0=Trade, 1=BidUpdate, 2=AskUpdate
    uint8_t   price_is_float; // 1 if price is native double (bit_cast'd into int64)
    uint8_t   _pad[6];      // Alignment padding
};
static_assert(sizeof(TickMessage) == 64, "TickMessage must be exactly 1 cache line");

// ============================================================================
// TickPlant: Pub/Sub 라우터
// ============================================================================
class TickPlant {
public:
    using TickQueue = MPMCRingBuffer<TickMessage, 65536>; // 64K slots

    TickPlant();

    /// 외부 데이터 수신 (NIC → TickPlant)
    /// 자동으로 seq_num 및 recv_ts 할당
    bool ingest(TickMessage msg);

    /// 구독자: RDB가 틱을 꺼내감
    std::optional<TickMessage> consume();

    /// 현재 시퀀스 번호
    [[nodiscard]] SeqNum current_seq() const {
        return seq_counter_.load(std::memory_order_relaxed);
    }

    /// 큐 상태
    [[nodiscard]] size_t queue_depth() const { return queue_.approx_size(); }

private:
    static Timestamp now_ns();

    TickQueue queue_;
    std::atomic<SeqNum> seq_counter_{0};
};

} // namespace zeptodb::ingestion

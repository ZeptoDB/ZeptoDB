// ============================================================================
// Layer 2: Tick Plant Implementation
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/common/logger.h"
#include <chrono>

namespace zeptodb::ingestion {

TickPlant::TickPlant() {
    ZEPTO_INFO("TickPlant initialized (queue capacity={})", TickQueue::capacity());
}

Timestamp TickPlant::now_ns() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
}

bool TickPlant::ingest(TickMessage msg) {
    // Stamp with global sequence and receive timestamp
    msg.seq_num = seq_counter_.fetch_add(1, std::memory_order_relaxed);
    msg.recv_ts = now_ns();

    if (!queue_.try_push(msg)) {
        ZEPTO_WARN("TickPlant queue full! Dropping tick seq={}", msg.seq_num);
        return false;
    }
    return true;
}

std::optional<TickMessage> TickPlant::consume() {
    return queue_.try_pop();
}

} // namespace zeptodb::ingestion

// ============================================================================
// APEX-DB: End-to-End Integration Pipeline — Implementation
// ============================================================================
// Layer 1 (Storage) + Layer 2 (Ingestion) + Layer 3 (Execution) 통합
//
// 아키텍처:
//   외부 -> ingest_tick() -> TickPlant (MPMC Queue)
//                              |
//                         [drain_thread]
//                              |
//                         store_tick() -> PartitionManager -> ColumnVectors
//                                                                 |
//                    query_vwap() / query_filter_sum() -----------+
//                              -> VectorizedEngine (벡터화 연산)
//
//   Tiered 모드:
//     FlushManager 백그라운드 스레드 → SEALED 파티션 → HDBWriter → 디스크
//     쿼리 시: RDB (메모리) + HDB (디스크 mmap) 통합 집계
// ============================================================================

#include "apex/core/pipeline.h"
#include "apex/common/logger.h"

#include <algorithm>
#include <chrono>

namespace apex::core {

// ============================================================================
// 스키마 상수: 파티션에 생성할 컬럼 이름
// ============================================================================
static constexpr const char* COL_TIMESTAMP = "timestamp";
static constexpr const char* COL_PRICE     = "price";
static constexpr const char* COL_VOLUME    = "volume";
static constexpr const char* COL_MSG_TYPE  = "msg_type";

// ============================================================================
// 내부 헬퍼: 고해상도 타이머 (nanosecond)
// ============================================================================
static inline int64_t pipeline_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// 생성자 / 소멸자
// ============================================================================
ApexPipeline::ApexPipeline(PipelineConfig config)
    : config_(config)
    , partition_mgr_(config.arena_size_per_partition)
{
    APEX_INFO("ApexPipeline 초기화 (arena={}MB, batch={}, mode={})",
              config.arena_size_per_partition / (1024*1024),
              config.drain_batch_size,
              static_cast<int>(config.storage_mode));

    // Tiered / Pure On-Disk 모드에서 HDB 컴포넌트 초기화
    if (config_.storage_mode == StorageMode::TIERED ||
        config_.storage_mode == StorageMode::PURE_ON_DISK) {

        const bool use_comp = config_.flush_config.enable_compression;
        hdb_writer_ = std::make_unique<HDBWriter>(config_.hdb_base_path, use_comp);
        hdb_reader_ = std::make_unique<HDBReader>(config_.hdb_base_path);

        if (config_.storage_mode == StorageMode::TIERED) {
            flush_manager_ = std::make_unique<FlushManager>(
                partition_mgr_,
                *hdb_writer_,
                config_.flush_config
            );
            APEX_INFO("FlushManager 생성됨 (Tiered 모드)");
        }

        APEX_INFO("HDB 활성화: path={}", config_.hdb_base_path);
    }
}

ApexPipeline::~ApexPipeline() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ============================================================================
// start / stop
// ============================================================================
void ApexPipeline::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
        APEX_WARN("ApexPipeline::start() — 이미 실행 중");
        return;
    }

    // FlushManager 시작 (Tiered 모드)
    if (flush_manager_) {
        flush_manager_->start();
    }

    size_t n_drain = std::max<size_t>(1, config_.drain_threads);
    for (size_t i = 0; i < n_drain; ++i)
        drain_threads_.emplace_back([this]() { drain_loop(); });
    APEX_INFO("ApexPipeline 시작 완료 (drain_threads={})", n_drain);
}

void ApexPipeline::stop() {
    running_.store(false, std::memory_order_release);

    if (flush_manager_) {
        flush_manager_->stop();
    }

    for (auto& t : drain_threads_) {
        if (t.joinable()) t.join();
    }
    drain_threads_.clear();

    // 남은 큐 아이템 동기 플러시
    const size_t remaining = drain_sync();
    APEX_INFO("ApexPipeline 중지 (잔여 flush={})", remaining);
}

// ============================================================================
// ingest_tick: 외부 틱 수신 (Thread-safe, lock-free)
// ============================================================================
bool ApexPipeline::ingest_tick(TickMessage msg) {
    const int64_t t0 = pipeline_now_ns();
    const bool ok = tick_plant_.ingest(msg);
    if (ok) {
        stats_.ticks_ingested.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Queue full — direct-to-storage bypass (slower but no data loss)
        store_tick(msg);
        stats_.ticks_ingested.fetch_add(1, std::memory_order_relaxed);
    }
    stats_.last_ingest_latency_ns.store(
        pipeline_now_ns() - t0, std::memory_order_relaxed);
    return ok;
}

// ============================================================================
// store_tick: 틱 → ColumnStore 저장 (드레인 스레드에서만 호출)
// ============================================================================
void ApexPipeline::store_tick(const TickMessage& msg) {
    // 파티션 가져오기 (없으면 자동 생성)
    Partition& partition = partition_mgr_.get_or_create(msg.symbol_id, msg.recv_ts);

    // 파티션 최초 접근 시 스키마 초기화
    if (partition.get_column(COL_TIMESTAMP) == nullptr) {
        partition.add_column(COL_TIMESTAMP, ColumnType::TIMESTAMP_NS);
        partition.add_column(COL_PRICE,     ColumnType::INT64);
        partition.add_column(COL_VOLUME,    ColumnType::INT64);
        partition.add_column(COL_MSG_TYPE,  ColumnType::INT32);

        // partition_index_ 업데이트
        {
            std::lock_guard<std::mutex> lk(partition_index_mu_);
            partition_index_[msg.symbol_id].push_back(&partition);
        }

        stats_.partitions_created.fetch_add(1, std::memory_order_relaxed);
        APEX_DEBUG("파티션 스키마 초기화: symbol={}", msg.symbol_id);
    }

    // 컬럼에 데이터 append
    partition.get_column(COL_TIMESTAMP)->append<int64_t>(msg.recv_ts);
    partition.get_column(COL_PRICE    )->append<int64_t>(msg.price);
    partition.get_column(COL_VOLUME   )->append<int64_t>(msg.volume);
    partition.get_column(COL_MSG_TYPE )->append<int32_t>(
        static_cast<int32_t>(msg.msg_type));

    stats_.ticks_stored.fetch_add(1, std::memory_order_relaxed);
}

// ============================================================================
// drain_loop: 백그라운드 드레인 스레드
// ============================================================================
void ApexPipeline::drain_loop() {
    APEX_DEBUG("드레인 스레드 시작");
    while (running_.load(std::memory_order_acquire)) {
        size_t drained = 0;
        for (size_t i = 0; i < config_.drain_batch_size; ++i) {
            auto msg = tick_plant_.consume();
            if (!msg.has_value()) break;
            store_tick(*msg);
            ++drained;
        }

        if (drained == 0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.drain_sleep_us));
        }
    }
    APEX_DEBUG("드레인 스레드 종료");
}

// ============================================================================
// drain_sync: 동기 드레인 (테스트/벤치용)
// ============================================================================
size_t ApexPipeline::drain_sync(size_t max_items) {
    size_t count = 0;
    while (count < max_items) {
        auto msg = tick_plant_.consume();
        if (!msg.has_value()) break;
        store_tick(*msg);
        ++count;
    }
    return count;
}

// ============================================================================
// find_partitions: symbol에 대한 모든 파티션 포인터 반환
// ============================================================================
std::vector<Partition*> ApexPipeline::find_partitions(SymbolId symbol) const {
    std::lock_guard<std::mutex> lk(partition_index_mu_);
    auto it = partition_index_.find(symbol);
    if (it == partition_index_.end()) return {};
    return it->second;
}

// ============================================================================
// build_snapshot: 파티션에서 ColumnSnapshot 빌드
// ============================================================================
ApexPipeline::ColumnSnapshot ApexPipeline::build_snapshot(
    Partition* part, const std::string& extra_col_name
) const {
    ColumnSnapshot snap;

    auto* ts_col  = part->get_column(COL_TIMESTAMP);
    auto* px_col  = part->get_column(COL_PRICE);
    auto* vol_col = part->get_column(COL_VOLUME);
    if (!ts_col || !px_col || !vol_col) return snap;

    snap.count      = ts_col->size();
    snap.timestamps = static_cast<const int64_t*>(ts_col->raw_data());
    snap.prices     = static_cast<const int64_t*>(px_col->raw_data());
    snap.volumes    = static_cast<const int64_t*>(vol_col->raw_data());

    if (!extra_col_name.empty()) {
        auto* col = part->get_column(extra_col_name);
        if (col) {
            snap.extra_col = static_cast<const int64_t*>(col->raw_data());
        }
    }

    return snap;
}

// ============================================================================
// hdb_count_range: HDB에서 시간 범위 내 COUNT 집계
// (Tiered 쿼리의 HDB 기여분 계산)
// ============================================================================
size_t ApexPipeline::hdb_count_range(SymbolId symbol,
                                      Timestamp from, Timestamp to) const {
    if (!hdb_reader_) return 0;

    const auto partitions = hdb_reader_->list_partitions_in_range(symbol, from, to);
    size_t total = 0;

    for (const int64_t hour : partitions) {
        auto ts_col = hdb_reader_->read_column(symbol, hour, COL_TIMESTAMP);
        if (!ts_col.valid()) continue;

        const auto ts_span = ts_col.as_span<int64_t>();
        for (const int64_t ts : ts_span) {
            if (ts >= from && ts <= to) {
                ++total;
            }
        }
    }

    return total;
}

// ============================================================================
// query_vwap: VWAP 쿼리
// ============================================================================
QueryResult ApexPipeline::query_vwap(
    SymbolId symbol, Timestamp from, Timestamp to
) {
    const int64_t t0 = pipeline_now_ns();

    const auto partitions = find_partitions(symbol);

    __int128 pv_sum    = 0;
    int64_t  v_sum     = 0;
    size_t   total_rows = 0;

    const bool full_scan = (from == 0 && to == INT64_MAX);

    // ===== RDB (in-memory) 스캔 =====
    for (Partition* part : partitions) {
        const auto snap = build_snapshot(part, "");
        if (snap.count == 0) continue;

        if (!full_scan) {
            if (snap.timestamps[snap.count - 1] < from) continue;
            if (snap.timestamps[0] > to) continue;
        }

        if (full_scan) {
            for (size_t i = 0; i < snap.count; ++i) {
                pv_sum += static_cast<__int128>(snap.prices[i]) * snap.volumes[i];
                v_sum  += snap.volumes[i];
            }
            total_rows += snap.count;
        } else {
            for (size_t i = 0; i < snap.count; ++i) {
                if (snap.timestamps[i] >= from && snap.timestamps[i] <= to) {
                    pv_sum += static_cast<__int128>(snap.prices[i]) * snap.volumes[i];
                    v_sum  += snap.volumes[i];
                    ++total_rows;
                }
            }
        }
    }

    // ===== HDB (disk mmap) 스캔 — Tiered / Pure On-Disk 모드 =====
    if (hdb_reader_ && partitions.empty()) {
        // RDB에 없을 때만 HDB 조회 (또는 Pure On-Disk 모드)
        const auto hdb_parts = hdb_reader_->list_partitions_in_range(symbol, from, to);
        for (const int64_t hour : hdb_parts) {
            auto ts_col  = hdb_reader_->read_column(symbol, hour, COL_TIMESTAMP);
            auto px_col  = hdb_reader_->read_column(symbol, hour, COL_PRICE);
            auto vol_col = hdb_reader_->read_column(symbol, hour, COL_VOLUME);

            if (!ts_col.valid() || !px_col.valid() || !vol_col.valid()) continue;

            const auto ts_span  = ts_col.as_span<int64_t>();
            const auto px_span  = px_col.as_span<int64_t>();
            const auto vol_span = vol_col.as_span<int64_t>();

            for (size_t i = 0; i < ts_col.num_rows; ++i) {
                if (full_scan || (ts_span[i] >= from && ts_span[i] <= to)) {
                    pv_sum += static_cast<__int128>(px_span[i]) * vol_span[i];
                    v_sum  += vol_span[i];
                    ++total_rows;
                }
            }
        }
    }

    if (total_rows == 0) {
        return QueryResult{
            .type = QueryResult::Type::ERROR,
            .error_msg = "no data for symbol"
        };
    }

    QueryResult r;
    r.type = QueryResult::Type::VWAP;
    r.value = (v_sum == 0) ? 0.0
              : static_cast<double>(pv_sum) / static_cast<double>(v_sum);
    r.rows_scanned = total_rows;
    r.latency_ns   = pipeline_now_ns() - t0;

    stats_.queries_executed.fetch_add(1, std::memory_order_relaxed);
    stats_.total_rows_scanned.fetch_add(total_rows, std::memory_order_relaxed);

    return r;
}

// ============================================================================
// query_filter_sum: Filter + Sum 쿼리
// ============================================================================
QueryResult ApexPipeline::query_filter_sum(
    SymbolId symbol,
    const std::string& column,
    int64_t threshold,
    Timestamp from,
    Timestamp to
) {
    const int64_t t0 = pipeline_now_ns();

    const auto partitions = find_partitions(symbol);

    int64_t total_sum  = 0;
    size_t  total_rows = 0;

    const bool full_scan = (from == 0 && to == INT64_MAX);

    // SelectionVector: DATABLOCK_ROWS 크기
    SelectionVector sel(DATABLOCK_ROWS);

    // ===== RDB 스캔 =====
    for (Partition* part : partitions) {
        const auto snap = build_snapshot(part, column);
        if (snap.count == 0) continue;

        const int64_t* col_data = nullptr;
        if (column == COL_PRICE) {
            col_data = snap.prices;
        } else if (column == COL_VOLUME) {
            col_data = snap.volumes;
        } else if (snap.extra_col) {
            col_data = snap.extra_col;
        } else {
            col_data = snap.prices;
        }

        if (!col_data) continue;

        if (full_scan) {
            size_t offset = 0;
            while (offset < snap.count) {
                const size_t block = std::min(DATABLOCK_ROWS, snap.count - offset);
                filter_gt_i64(col_data + offset, block, threshold, sel);
                total_sum  += sum_i64_selected(col_data + offset, sel);
                total_rows += sel.size();
                offset     += block;
            }
        } else {
            for (size_t i = 0; i < snap.count; ++i) {
                if (snap.timestamps[i] >= from && snap.timestamps[i] <= to) {
                    if (col_data[i] > threshold) {
                        total_sum += col_data[i];
                        ++total_rows;
                    }
                }
            }
        }
    }

    // ===== HDB 스캔 — Tiered / Pure On-Disk 모드 =====
    if (hdb_reader_ && partitions.empty()) {
        const auto hdb_parts = hdb_reader_->list_partitions_in_range(symbol, from, to);
        for (const int64_t hour : hdb_parts) {
            auto ts_col  = hdb_reader_->read_column(symbol, hour, COL_TIMESTAMP);
            auto tgt_col = hdb_reader_->read_column(symbol, hour, column);
            if (!ts_col.valid() || !tgt_col.valid()) continue;

            const auto ts_span  = ts_col.as_span<int64_t>();
            const auto col_span = tgt_col.as_span<int64_t>();

            for (size_t i = 0; i < ts_col.num_rows; ++i) {
                if (full_scan || (ts_span[i] >= from && ts_span[i] <= to)) {
                    if (col_span[i] > threshold) {
                        total_sum += col_span[i];
                        ++total_rows;
                    }
                }
            }
        }
    }

    QueryResult r;
    r.type     = QueryResult::Type::SUM;
    r.ivalue   = total_sum;
    r.value    = static_cast<double>(total_sum);
    r.rows_scanned = total_rows;
    r.latency_ns   = pipeline_now_ns() - t0;

    stats_.queries_executed.fetch_add(1, std::memory_order_relaxed);
    stats_.total_rows_scanned.fetch_add(total_rows, std::memory_order_relaxed);

    return r;
}

// ============================================================================
// query_count
// ============================================================================
QueryResult ApexPipeline::query_count(
    SymbolId symbol, Timestamp from, Timestamp to
) {
    const int64_t t0 = pipeline_now_ns();

    const auto partitions = find_partitions(symbol);
    size_t total = 0;

    const bool full_scan = (from == 0 && to == INT64_MAX);

    // RDB 스캔
    for (Partition* part : partitions) {
        const auto snap = build_snapshot(part, "");
        if (snap.count == 0) continue;

        if (full_scan) {
            total += snap.count;
        } else {
            for (size_t i = 0; i < snap.count; ++i) {
                if (snap.timestamps[i] >= from && snap.timestamps[i] <= to) {
                    ++total;
                }
            }
        }
    }

    // HDB 스캔 (Tiered / Pure On-Disk 모드)
    if (hdb_reader_) {
        total += hdb_count_range(symbol, from, to);
    }

    QueryResult r;
    r.type         = QueryResult::Type::COUNT;
    r.ivalue       = static_cast<int64_t>(total);
    r.value        = static_cast<double>(total);
    r.rows_scanned = total;
    r.latency_ns   = pipeline_now_ns() - t0;

    stats_.queries_executed.fetch_add(1, std::memory_order_relaxed);
    stats_.total_rows_scanned.fetch_add(total, std::memory_order_relaxed);

    return r;
}

// ============================================================================
// total_stored_rows
// ============================================================================
size_t ApexPipeline::total_stored_rows() const {
    std::lock_guard<std::mutex> lk(partition_index_mu_);
    size_t total = 0;
    for (const auto& [sym, parts] : partition_index_) {
        for (const Partition* p : parts) {
            total += p->num_rows();
        }
    }
    return total;
}

} // namespace apex::core


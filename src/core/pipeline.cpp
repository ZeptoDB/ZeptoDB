// ============================================================================
// ZeptoDB: End-to-End Integration Pipeline — Implementation
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

#include "zeptodb/core/pipeline.h"
#include "zeptodb/common/logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace zeptodb::core {

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
ZeptoPipeline::ZeptoPipeline(PipelineConfig config)
    : config_(config)
    , partition_mgr_(config.arena_size_per_partition)
{
    ZEPTO_INFO("ZeptoPipeline 초기화 (arena={}MB, batch={}, mode={})",
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
            ZEPTO_INFO("FlushManager 생성됨 (Tiered 모드)");
        }

        ZEPTO_INFO("HDB 활성화: path={}", config_.hdb_base_path);
    }

    // Load persisted schema catalog if present (HDB modes only).
    if (!config_.hdb_base_path.empty()) {
        const std::string schema_path = config_.hdb_base_path + "/_schema.json";
        if (schema_registry_.load_from(schema_path)) {
            ZEPTO_INFO("SchemaRegistry loaded from {} ({} tables)",
                      schema_path, schema_registry_.table_count());
        }
    }
}

ZeptoPipeline::~ZeptoPipeline() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }

    // Ensure deterministic teardown order regardless of whether start() was
    // called: FlushManager references PartitionManager & HDBWriter, so it
    // must be destroyed before them.  Clearing partition_index_ avoids
    // dangling pointers during PartitionManager destruction.
    flush_manager_.reset();
    {
        std::lock_guard<std::mutex> lk(partition_index_mu_);
        partition_index_.clear();
    }
}

// ============================================================================
// start / stop
// ============================================================================
void ZeptoPipeline::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
        ZEPTO_WARN("ZeptoPipeline::start() — 이미 실행 중");
        return;
    }

    // Recovery: reload in-memory data from snapshot directory
    if (config_.enable_recovery && !config_.recovery_snapshot_path.empty()) {
        const std::string& snap = config_.recovery_snapshot_path;
        namespace fs = std::filesystem;
        size_t recovered_rows = 0;

        std::error_code ec;
        if (fs::exists(snap, ec)) {
            HDBReader snap_reader(snap);

            // Walk one symbol subtree (`base/{sym}/{hour}/...`) and reload rows.
            auto walk_symbol = [&](const fs::path& sym_path, uint16_t table_id) {
                SymbolId sym_id = 0;
                try { sym_id = static_cast<SymbolId>(
                    std::stoll(sym_path.filename().string())); }
                catch (...) { return; }

                std::error_code ec2;
                for (auto& hour_entry : fs::directory_iterator(sym_path, ec2)) {
                    if (!hour_entry.is_directory()) continue;
                    int64_t hour_epoch = 0;
                    try { hour_epoch = std::stoll(
                        hour_entry.path().filename().string()); }
                    catch (...) { continue; }

                    auto ts_col  = snap_reader.read_column(table_id, sym_id, hour_epoch, COL_TIMESTAMP);
                    auto px_col  = snap_reader.read_column(table_id, sym_id, hour_epoch, COL_PRICE);
                    auto vol_col = snap_reader.read_column(table_id, sym_id, hour_epoch, COL_VOLUME);
                    auto mt_col  = snap_reader.read_column(table_id, sym_id, hour_epoch, COL_MSG_TYPE);

                    if (!ts_col.valid() || !px_col.valid() || !vol_col.valid()) {
                        ZEPTO_WARN("Recovery: incomplete snapshot for table={} symbol={} hour={}",
                                  table_id, sym_id, hour_epoch);
                        continue;
                    }

                    const size_t n = ts_col.num_rows;
                    auto ts_span  = ts_col.as_span<int64_t>();
                    auto px_span  = px_col.as_span<int64_t>();
                    auto vol_span = vol_col.as_span<int64_t>();

                    for (size_t i = 0; i < n; ++i) {
                        TickMessage msg{};
                        msg.table_id  = table_id;
                        msg.symbol_id = sym_id;
                        msg.recv_ts   = ts_span[i];
                        msg.price     = px_span[i];
                        msg.volume    = vol_span[i];
                        if (mt_col.valid()) {
                            msg.msg_type = static_cast<uint8_t>(
                                mt_col.as_span<int32_t>()[i]);
                        }
                        store_tick(msg);
                        ++recovered_rows;
                    }
                }
            };

            for (auto& entry : fs::directory_iterator(snap, ec)) {
                if (!entry.is_directory()) continue;
                const std::string name = entry.path().filename().string();
                // Table-scoped subtree: `t{table_id}/...`
                if (name.size() > 1 && name[0] == 't' &&
                    std::all_of(name.begin() + 1, name.end(),
                                [](char c){ return std::isdigit(static_cast<unsigned char>(c)); })) {
                    uint16_t tid = 0;
                    try { tid = static_cast<uint16_t>(std::stoi(name.substr(1))); }
                    catch (...) { continue; }
                    std::error_code ec3;
                    for (auto& sym_entry : fs::directory_iterator(entry.path(), ec3)) {
                        if (!sym_entry.is_directory()) continue;
                        walk_symbol(sym_entry.path(), tid);
                    }
                } else {
                    // Legacy layout `{sym}/{hour}/...` — table_id = 0.
                    walk_symbol(entry.path(), 0);
                }
            }
        }

        ZEPTO_INFO("Recovery complete: {} rows reloaded from {}", recovered_rows, snap);
    }

    // FlushManager 시작 (Tiered 모드)
    if (flush_manager_) {
        flush_manager_->start();
    }

    size_t n_drain = std::max<size_t>(1, config_.drain_threads);
    for (size_t i = 0; i < n_drain; ++i)
        drain_threads_.emplace_back([this]() { drain_loop(); });
    ZEPTO_INFO("ZeptoPipeline 시작 완료 (drain_threads={})", n_drain);
}

void ZeptoPipeline::stop() {
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
    ZEPTO_INFO("ZeptoPipeline 중지 (잔여 flush={})", remaining);
}

// ============================================================================
// ingest_tick: 외부 틱 수신 (Thread-safe, lock-free)
// ============================================================================
bool ZeptoPipeline::ingest_tick(TickMessage msg) {
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

size_t ZeptoPipeline::ingest_batch(int32_t symbol,
                                    const int64_t* prices,
                                    const int64_t* volumes,
                                    const int64_t* timestamps,
                                    size_t count) {
    int64_t ts = timestamps ? 0 : pipeline_now_ns();
    size_t queued = 0;
    for (size_t i = 0; i < count; ++i) {
        TickMessage msg{};
        msg.symbol_id = symbol;
        msg.price     = prices[i];
        msg.volume    = volumes[i];
        msg.recv_ts   = timestamps ? timestamps[i] : (ts + static_cast<int64_t>(i));
        msg.msg_type  = 0;
        ingest_tick(msg);
        ++queued;
    }
    return queued;
}

// ============================================================================
// store_tick: 틱 → ColumnStore 저장 (드레인 스레드에서만 호출)
// ============================================================================
void ZeptoPipeline::store_tick(const TickMessage& msg) {
    // 파티션 가져오기 (없으면 자동 생성) — table-scoped
    Partition& partition = partition_mgr_.get_or_create(msg.table_id, msg.symbol_id, msg.recv_ts);

    // 파티션 최초 접근 시 스키마 초기화
    if (partition.get_column(COL_TIMESTAMP) == nullptr) {
        partition.add_column(COL_TIMESTAMP, ColumnType::TIMESTAMP_NS);
        partition.add_column(COL_PRICE,     msg.price_is_float ? ColumnType::FLOAT64
                                                               : ColumnType::INT64);
        partition.add_column(COL_VOLUME,    ColumnType::INT64);
        partition.add_column(COL_MSG_TYPE,  ColumnType::INT32);

        // partition_index_ 업데이트 — key = (table_id << 32) | symbol_id
        {
            std::lock_guard<std::mutex> lk(partition_index_mu_);
            const uint64_t k = (static_cast<uint64_t>(msg.table_id) << 32)
                             | static_cast<uint32_t>(msg.symbol_id);
            partition_index_[k].push_back(&partition);
        }

        stats_.partitions_created.fetch_add(1, std::memory_order_relaxed);
        ZEPTO_DEBUG("파티션 스키마 초기화: symbol={}", msg.symbol_id);
    }

    // 컬럼에 데이터 append (타입 디스패치) — cache column pointers once
    // to avoid repeated linear scans in Partition::get_column (memcmp hotspot).
    ColumnVector* ts_col  = partition.get_column(COL_TIMESTAMP);
    ColumnVector* px_col  = partition.get_column(COL_PRICE);
    ColumnVector* vol_col = partition.get_column(COL_VOLUME);
    ColumnVector* mt_col  = partition.get_column(COL_MSG_TYPE);
    ts_col->append<int64_t>(msg.recv_ts);
    if (px_col->type() == ColumnType::FLOAT64)
        px_col->append<double>(msg.price_f);
    else
        px_col->append<int64_t>(msg.price);
    vol_col->append<int64_t>(msg.volume);
    mt_col->append<int32_t>(static_cast<int32_t>(msg.msg_type));

    stats_.ticks_stored.fetch_add(1, std::memory_order_relaxed);

    // Materialized view incremental update
    mat_view_mgr_.on_tick(msg.symbol_id, msg.recv_ts, msg.price, msg.volume);

    // P0-6: Memory limit check — evict oldest partition if over limit
    if (config_.max_memory_bytes > 0) {
        size_t total = partition_mgr_.total_memory_bytes();
        if (total > config_.max_memory_bytes) {
            auto* oldest = partition_mgr_.oldest_partition();
            if (oldest && oldest != &partition) {
                partition_mgr_.evict_partition(oldest);
                stats_.partitions_evicted.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ============================================================================
// drain_loop: 백그라운드 드레인 스레드
// ============================================================================
void ZeptoPipeline::drain_loop() {
    ZEPTO_DEBUG("드레인 스레드 시작");
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
    ZEPTO_DEBUG("드레인 스레드 종료");
}

// ============================================================================
// drain_sync: 동기 드레인 (테스트/벤치용)
// ============================================================================
size_t ZeptoPipeline::drain_sync(size_t max_items) {
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
// find_partitions: (table_id, symbol)에 대한 모든 파티션 포인터 반환
// ============================================================================
std::vector<Partition*> ZeptoPipeline::find_partitions(uint16_t table_id, SymbolId symbol) const {
    std::lock_guard<std::mutex> lk(partition_index_mu_);
    const uint64_t k = (static_cast<uint64_t>(table_id) << 32)
                     | static_cast<uint32_t>(symbol);
    auto it = partition_index_.find(k);
    if (it == partition_index_.end()) return {};
    return it->second;
}

// ============================================================================
// build_snapshot: 파티션에서 ColumnSnapshot 빌드
// ============================================================================
ZeptoPipeline::ColumnSnapshot ZeptoPipeline::build_snapshot(
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
size_t ZeptoPipeline::hdb_count_range(SymbolId symbol,
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
QueryResult ZeptoPipeline::query_vwap(
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
    if (hdb_reader_ && partitions.empty()) [[unlikely]] {
        query_vwap_hdb_fallback(symbol, from, to, pv_sum, v_sum, total_rows);
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
// query_vwap_hdb_fallback: Cold HDB fallback (extracted to reduce hot-path size)
// ============================================================================
[[gnu::cold, gnu::noinline]]
void ZeptoPipeline::query_vwap_hdb_fallback(
    SymbolId symbol, Timestamp from, Timestamp to,
    __int128& pv_sum, int64_t& v_sum, size_t& total_rows) const {
    const bool full_scan = (from == 0 && to == INT64_MAX);
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

// ============================================================================
// query_filter_sum: Filter + Sum 쿼리
// ============================================================================
QueryResult ZeptoPipeline::query_filter_sum(
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
QueryResult ZeptoPipeline::query_count(
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
// ============================================================================
// evict_older_than_ns: TTL eviction + partition_index_ rebuild
// ============================================================================
size_t ZeptoPipeline::evict_older_than_ns(int64_t cutoff_ns) {
    const size_t evicted = partition_mgr_.evict_older_than(cutoff_ns);
    if (evicted > 0) {
        // Rebuild partition_index_ to eliminate stale raw pointers.
        std::lock_guard<std::mutex> lk(partition_index_mu_);
        partition_index_.clear();
        for (Partition* p : partition_mgr_.get_all_partitions()) {
            const auto& k = p->key();
            const uint64_t idx_key = (static_cast<uint64_t>(k.table_id) << 32)
                                   | static_cast<uint32_t>(k.symbol_id);
            partition_index_[idx_key].push_back(p);
        }
    }
    return evicted;
}

// ============================================================================
// drop_table_index: remove partition_index_ entries for a given table_id
// ============================================================================
void ZeptoPipeline::drop_table_index(uint16_t table_id) {
    std::lock_guard<std::mutex> lk(partition_index_mu_);
    for (auto it = partition_index_.begin(); it != partition_index_.end(); ) {
        if (static_cast<uint16_t>(it->first >> 32) == table_id) {
            it = partition_index_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// total_stored_rows
// ============================================================================
size_t ZeptoPipeline::total_stored_rows() const {
    std::lock_guard<std::mutex> lk(partition_index_mu_);
    size_t total = 0;
    for (const auto& [sym, parts] : partition_index_) {
        for (const Partition* p : parts) {
            total += p->num_rows();
        }
    }
    return total;
}

} // namespace zeptodb::core


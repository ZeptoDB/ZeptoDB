// ============================================================================
// Layer 1: FlushManager — 구현
// ============================================================================

#include "zeptodb/storage/flush_manager.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include <unistd.h>

namespace zeptodb::storage {

namespace {

namespace fs = std::filesystem;

constexpr const char* kSnapshotManifestMagic = "ZEPTO_SNAPSHOT_V1";
std::atomic<uint64_t> g_snapshot_generation_sequence{0};

bool is_generation_name(const std::string& name) {
    if (!name.starts_with("gen-") || name.size() <= 4 || name.size() > 160) {
        return false;
    }
    return std::all_of(name.begin() + 4, name.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || ch == '-';
    });
}

bool write_text_atomically(const fs::path& destination,
                           const std::string& contents) {
    const uint64_t sequence = g_snapshot_generation_sequence.fetch_add(
        1, std::memory_order_relaxed);
    const fs::path temporary = destination.string() + ".tmp." +
        std::to_string(static_cast<uint64_t>(::getpid())) + "." +
        std::to_string(sequence);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        return false;
    }
    output.close();
    if (!output) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        return false;
    }

    std::error_code rename_error;
    fs::rename(temporary, destination, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        return false;
    }
    return true;
}

void cleanup_old_generations(const fs::path& generations_root,
                             const std::string& current_generation) {
    std::error_code ec;
    std::vector<fs::path> generations;
    for (const auto& entry : fs::directory_iterator(generations_root, ec)) {
        if (ec) return;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec) && !ec && is_generation_name(name)) {
            generations.push_back(entry.path());
        }
        ec.clear();
    }
    std::sort(generations.begin(), generations.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.filename().string() > right.filename().string();
              });

    size_t retained = 0;
    for (const auto& generation : generations) {
        const bool is_current =
            generation.filename().string() == current_generation;
        if (is_current || retained < 1) {
            if (!is_current) ++retained;
            continue;
        }
        std::error_code remove_error;
        fs::remove_all(generation, remove_error);
    }
}

SnapshotResolution invalid_snapshot(std::string error) {
    SnapshotResolution result;
    result.status = SnapshotResolutionStatus::Invalid;
    result.error = std::move(error);
    return result;
}

}  // namespace

SnapshotResolution FlushManager::resolve_snapshot(
    const std::string& snapshot_path) {
    if (snapshot_path.empty()) {
        return {};
    }

    const fs::path root(snapshot_path);
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        if (ec) {
            return invalid_snapshot("cannot inspect snapshot root: " + ec.message());
        }
        return {};
    }
    if (!fs::is_directory(root, ec) || ec) {
        return invalid_snapshot("snapshot root is not a directory");
    }

    const fs::path current_path = root / "CURRENT";
    if (!fs::exists(current_path, ec)) {
        if (ec) {
            return invalid_snapshot("cannot inspect snapshot CURRENT: " + ec.message());
        }

        bool has_unpublished_content = false;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) {
                return invalid_snapshot("cannot enumerate snapshot root: " + ec.message());
            }
            if (entry.path().filename() == "generations" &&
                entry.is_directory(ec) && !ec && fs::is_empty(entry.path(), ec) && !ec) {
                continue;
            }
            has_unpublished_content = true;
            break;
        }
        if (ec) {
            return invalid_snapshot("cannot inspect snapshot root: " + ec.message());
        }
        return has_unpublished_content
            ? invalid_snapshot("snapshot root has data but no CURRENT manifest")
            : SnapshotResolution{};
    }
    const auto current_status = fs::symlink_status(current_path, ec);
    if (ec || !fs::is_regular_file(current_status)) {
        return invalid_snapshot("snapshot CURRENT is not a regular file");
    }
    const auto current_size = fs::file_size(current_path, ec);
    if (ec || current_size == 0 || current_size > 512) {
        return invalid_snapshot("snapshot CURRENT has an invalid size");
    }

    std::ifstream current(current_path, std::ios::binary);
    std::string generation_name;
    std::string extra;
    if (!current || !(current >> generation_name) || (current >> extra) ||
        !is_generation_name(generation_name)) {
        return invalid_snapshot("snapshot CURRENT is malformed");
    }

    const fs::path generation_path =
        root / "generations" / generation_name;
    const auto generation_status = fs::symlink_status(generation_path, ec);
    if (ec || !fs::is_directory(generation_status)) {
        return invalid_snapshot("snapshot CURRENT generation is missing");
    }

    const fs::path complete_path = generation_path / "_COMPLETE";
    const auto complete_status = fs::symlink_status(complete_path, ec);
    if (ec || !fs::is_regular_file(complete_status)) {
        return invalid_snapshot("snapshot generation is not complete");
    }
    const auto complete_size = fs::file_size(complete_path, ec);
    if (ec || complete_size == 0 || complete_size > 512) {
        return invalid_snapshot("snapshot complete manifest has an invalid size");
    }

    std::ifstream complete(complete_path, std::ios::binary);
    std::string magic;
    std::string manifest_generation;
    uint64_t partition_count = 0;
    uint64_t row_count = 0;
    if (!complete || !(complete >> magic >> manifest_generation >> partition_count >> row_count) ||
        (complete >> extra) || magic != kSnapshotManifestMagic ||
        manifest_generation != generation_name ||
        partition_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        row_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return invalid_snapshot("snapshot complete manifest is malformed");
    }

    SnapshotResolution result;
    result.status = SnapshotResolutionStatus::Ready;
    result.generation_path = generation_path.string();
    result.partition_count = static_cast<size_t>(partition_count);
    result.row_count = static_cast<size_t>(row_count);
    return result;
}

// ============================================================================
// 생성자 / 소멸자
// ============================================================================
FlushManager::FlushManager(PartitionManager& pm,
                            HDBWriter&         writer,
                            FlushConfig        config,
                            EvictBeforeCallback evict_before)
    : pm_(pm)
    , writer_(writer)
    , config_(config)
    , evict_before_(std::move(evict_before))
{
    // Parquet writer 초기화 (필요 시)
    if (config_.output_format == HDBOutputFormat::PARQUET ||
        config_.output_format == HDBOutputFormat::BOTH) {
        parquet_writer_ = std::make_unique<ParquetWriter>(config_.parquet_config);
    }

    // S3 sink 초기화 (필요 시)
    if (config_.enable_s3_upload && !config_.s3_config.bucket.empty()) {
        s3_sink_ = std::make_unique<S3Sink>(config_.s3_config);
    } else if (config_.enable_s3_upload) {
        ZEPTO_WARN("FlushManager: S3 업로드 활성화됐지만 bucket이 비어있음 — 비활성화");
    }

    ZEPTO_INFO("FlushManager 초기화: threshold={:.0f}%, interval={}ms, "
              "compression={}, format={}, s3={}",
              config_.memory_threshold * 100.0,
              config_.check_interval_ms,
              config_.enable_compression ? "LZ4" : "OFF",
              config_.output_format == HDBOutputFormat::BINARY   ? "BINARY"  :
              config_.output_format == HDBOutputFormat::PARQUET  ? "PARQUET" : "BOTH",
              s3_sink_ ? config_.s3_config.bucket : "disabled");
}

FlushManager::~FlushManager() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ============================================================================
// start / stop
// ============================================================================
void FlushManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
        ZEPTO_WARN("FlushManager::start() — 이미 실행 중");
        return;
    }

    flush_thread_ = std::thread([this]() { flush_loop(); });
    ZEPTO_INFO("FlushManager 백그라운드 스레드 시작");
}

void FlushManager::stop() {
    running_.store(false, std::memory_order_release);
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    ZEPTO_INFO("FlushManager 중지 (총 플러시={}, 총 바이트={})",
              stat_partitions_flushed_.load(),
              stat_bytes_written_.load());
}

// ============================================================================
// flush_loop: 백그라운드 모니터링 루프
// ============================================================================
void FlushManager::flush_loop() {
    ZEPTO_DEBUG("FlushManager 루프 시작");

    while (running_.load(std::memory_order_acquire)) {
        // 인터벌 대기 (1ms 단위 폴링으로 stop() 반응성 확보)
        const uint32_t total_ms = config_.check_interval_ms;
        uint32_t waited = 0;
        while (waited < total_ms && running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++waited;
        }

        if (!running_.load(std::memory_order_acquire)) break;

        // 현재 타임스탬프로 오래된 파티션 자동 봉인
        const int64_t current_ts = now_ns();
        pm_.seal_old_partitions(current_ts, config_.auto_seal_age_hours);

        // 메모리 압력 확인 후 플러시 여부 결정
        // PartitionManager에 직접 접근 불가하므로 HDBWriter stats 기반으로 판단
        // 실제 프로덕션에선 arena utilization을 집계해야 함
        // 여기서는 항상 SEALED 파티션을 플러시하는 단순 전략 사용
        const size_t flushed = do_flush_sealed();
        if (flushed > 0) {
            stat_flush_triggers_.fetch_add(1, std::memory_order_relaxed);
            ZEPTO_DEBUG("FlushManager: auto-flush {} partitions", flushed);
        }

        // TTL-based partition eviction
        const int64_t ttl = ttl_ns_.load(std::memory_order_relaxed);
        if (ttl > 0) {
            const int64_t cutoff = now_ns() - ttl;
            const size_t evicted = evict_before_
                ? evict_before_(cutoff)
                : pm_.evict_older_than(cutoff);
            if (evicted > 0) {
                ZEPTO_INFO("FlushManager: TTL evicted {} partitions older than {}ns",
                          evicted, ttl);
            }
        }

        // Auto-snapshot timer check
        if (config_.enable_auto_snapshot && !config_.snapshot_path.empty()) {
            const int64_t interval_ns =
                static_cast<int64_t>(config_.snapshot_interval_ms) * 1'000'000LL;
            const int64_t last = last_snapshot_ns_.load(std::memory_order_relaxed);
            if (now_ns() - last >= interval_ns) {
                do_snapshot();
            }
        }

        // Storage tiering: Hot → Warm → Cold → Drop
        if (config_.tiering.enabled) {
            do_tiering();
        }
    }

    ZEPTO_DEBUG("FlushManager loop exit");
}

// ============================================================================
// flush_now: 수동 즉시 플러시
// ============================================================================
size_t FlushManager::flush_now() {
    ZEPTO_INFO("FlushManager::flush_now() 수동 트리거");
    const size_t flushed = do_flush_sealed();
    stat_manual_flushes_.fetch_add(1, std::memory_order_relaxed);
    return flushed;
}

// ============================================================================
// do_flush_sealed: 내부 공통 플러시 로직
// ============================================================================
size_t FlushManager::do_flush_sealed() {
    // PartitionManager에서 SEALED 파티션 목록 가져와서 처리
    // PartitionManager의 sealed_partitions()가 없으므로
    // seal_old_partitions(0, 0) 방식으로 전체 SEALED 확인은 불가능
    // → PartitionManager에 새 메서드 필요

    // SEALED 파티션 수집 (PartitionManager public API 활용)
    // 현재 API: seal_old_partitions()은 새로 봉인된 것만 반환
    // 기존에 SEALED된 것들은 partitions_ 맵을 직접 순회해야 함
    // → 여기서는 get_sealed_partitions() 확장 메서드를 사용
    auto sealed = pm_.get_sealed_partitions();

    size_t count = 0;
    for (Partition* part : sealed) {
        if (!part) continue;

        // FLUSHING 상태로 전이 (단일 flush 스레드)
        part->set_state(Partition::State::FLUSHING);

        size_t bytes = 0;

        // --- BINARY 저장 ---
        if (config_.output_format == HDBOutputFormat::BINARY ||
            config_.output_format == HDBOutputFormat::BOTH) {
            bytes = writer_.flush_partition(*part);
        }

        // --- PARQUET 저장 + 선택적 S3 업로드 ---
        if (config_.output_format == HDBOutputFormat::PARQUET ||
            config_.output_format == HDBOutputFormat::BOTH) {
            flush_partition_parquet(*part);
            // PARQUET 전용 모드: Parquet 쓰기 성공 시 bytes 설정
            if (config_.output_format == HDBOutputFormat::PARQUET &&
                parquet_writer_ && parquet_writer_->files_written() > 0) {
                bytes = parquet_writer_->bytes_written();
            }
        }

        const bool success = (bytes > 0) ||
            (parquet_writer_ && parquet_writer_->files_written() > 0) ||
            (s3_sink_        && s3_sink_->uploads_succeeded() > 0);

        if (success) {
            part->set_state(Partition::State::FLUSHED);
            if (config_.reclaim_after_flush) {
                part->reclaim_arena();
            }
            stat_partitions_flushed_.fetch_add(1, std::memory_order_relaxed);
            stat_bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
            stat_last_flush_ns_.store(now_ns(), std::memory_order_relaxed);
            ++count;
        } else {
            ZEPTO_WARN("do_flush_sealed: 플러시 실패, SEALED 상태 복원");
            part->set_state(Partition::State::SEALED);
        }
    }

    return count;
}

// ============================================================================
// snapshot_now / do_snapshot: all partitions (including ACTIVE) → snapshot_path
// ============================================================================
size_t FlushManager::snapshot_now() {
    ZEPTO_INFO("FlushManager::snapshot_now() triggered");
    return do_snapshot();
}

size_t FlushManager::do_snapshot() {
    std::lock_guard<std::mutex> snapshot_lock(snapshot_mu_);
    last_snapshot_succeeded_.store(false, std::memory_order_release);
    if (config_.snapshot_path.empty()) return 0;

    const fs::path root(config_.snapshot_path);
    const fs::path generations_root = root / "generations";
    std::error_code ec;
    fs::create_directories(generations_root, ec);
    if (ec) {
        ZEPTO_WARN("FlushManager: cannot create snapshot root {} ({})",
                   generations_root.string(), ec.message());
        return 0;
    }

    const uint64_t sequence = g_snapshot_generation_sequence.fetch_add(
        1, std::memory_order_relaxed);
    const std::string generation_name = "gen-" + std::to_string(now_ns()) + "-" +
        std::to_string(static_cast<uint64_t>(::getpid())) + "-" +
        std::to_string(sequence);
    const fs::path generation_path = generations_root / generation_name;
    if (!fs::create_directory(generation_path, ec) || ec) {
        ZEPTO_WARN("FlushManager: cannot create snapshot generation {} ({})",
                   generation_path.string(), ec.message());
        return 0;
    }

    size_t partition_count = 0;
    size_t row_count = 0;
    const bool wrote_all = pm_.visit_partitions_stable([&](Partition& partition) {
        const auto result = writer_.snapshot_partition_checked(
            partition, generation_path.string());
        if (!result.ok) {
            return false;
        }
        if (result.rows_written > 0) {
            if (row_count > std::numeric_limits<size_t>::max() -
                            result.rows_written ||
                partition_count == std::numeric_limits<size_t>::max()) {
                return false;
            }
            ++partition_count;
            row_count += result.rows_written;
        }
        return true;
    });

    if (!wrote_all) {
        fs::remove_all(generation_path, ec);
        ZEPTO_WARN("FlushManager: snapshot generation write failed: {}",
                   generation_path.string());
        return 0;
    }

    const std::string complete_contents =
        std::string(kSnapshotManifestMagic) + " " + generation_name + " " +
        std::to_string(partition_count) + " " + std::to_string(row_count) + "\n";
    if (!write_text_atomically(generation_path / "_COMPLETE", complete_contents) ||
        !write_text_atomically(root / "CURRENT", generation_name + "\n")) {
        fs::remove_all(generation_path, ec);
        ZEPTO_WARN("FlushManager: snapshot manifest publish failed: {}",
                   generation_path.string());
        return 0;
    }

    cleanup_old_generations(generations_root, generation_name);
    last_snapshot_ns_.store(now_ns(), std::memory_order_relaxed);
    last_snapshot_succeeded_.store(true, std::memory_order_release);
    ZEPTO_INFO("FlushManager: published snapshot generation {} "
               "({} partitions, {} rows)",
               generation_name, partition_count, row_count);
    return partition_count;
}

// ============================================================================
// flush_partition_parquet: 단일 파티션 Parquet 저장 + S3 업로드
// ============================================================================
void FlushManager::flush_partition_parquet(const Partition& partition)
{
    if (!parquet_writer_) return;

    const auto& key = partition.key();

    // 파티션 디렉토리 (table-scoped):
    //   table_id != 0 → {hdb_base}/t{tid}/{symbol}/{hour}
    //   table_id == 0 → {hdb_base}/{symbol}/{hour}   (legacy layout)
    const std::string parquet_dir = (key.table_id != 0)
        ? (writer_.base_path() + "/t" + std::to_string(key.table_id) + "/" +
           std::to_string(key.symbol_id) + "/" +
           std::to_string(key.hour_epoch))
        : (writer_.base_path() + "/" +
           std::to_string(key.symbol_id) + "/" +
           std::to_string(key.hour_epoch));

    const std::string filepath = parquet_writer_->flush_to_file(partition, parquet_dir);

    if (filepath.empty()) {
        ZEPTO_WARN("flush_partition_parquet: Parquet 쓰기 실패 (symbol={}, hour={})",
                  key.symbol_id, key.hour_epoch);
        return;
    }

    // S3 업로드
    if (s3_sink_) {
        // Partition `key.hour_epoch` is nanoseconds at hour boundary
        // (PartitionManager::to_hour_epoch). HIVE layout interprets the
        // make_s3_key arg as *hours since epoch UTC* (spec) so it can
        // emit `year=YYYY/month=MM/day=DD/...`. Convert here; FLAT keeps
        // the byte-identical pre-118 behaviour (passes ns through).
        const int64_t hour_for_s3 =
            (s3_sink_->config().layout == S3Layout::HIVE)
                ? (key.hour_epoch / (3600LL * 1'000'000'000LL))
                : key.hour_epoch;

        const std::string s3_key = s3_sink_->make_s3_key(
            key.symbol_id, hour_for_s3, "parquet");

        const bool uploaded = s3_sink_->upload_file(filepath, s3_key);

        if (uploaded) {
            ZEPTO_INFO("S3 업로드: {} → {}",
                      filepath, s3_sink_->make_s3_uri(s3_key));

            // 로컬 파일 삭제 (S3 업로드 후 스토리지 절약)
            if (config_.delete_local_after_s3) {
                std::error_code ec;
                std::filesystem::remove(filepath, ec);
                if (ec) {
                    ZEPTO_WARN("로컬 Parquet 삭제 실패: {} ({})", filepath, ec.message());
                }
            }
        }
    }
}

// ============================================================================
// do_tiering: age-based storage tier promotion
//   HOT  (ACTIVE, in-memory) → WARM (SEALED+FLUSHED, local SSD)
//   WARM (FLUSHED, local)    → COLD (S3 Parquet, local deleted)
//   COLD (S3 only)           → DROP (evict partition metadata)
// ============================================================================
void FlushManager::do_tiering() {
    const auto& tp = config_.tiering;
    const int64_t current = now_ns();
    auto all = pm_.get_all_partitions();

    for (Partition* part : all) {
        if (!part || part->num_rows() == 0) continue;

        const int64_t age = current - part->key().hour_epoch;

        // DROP tier: evict partition entirely
        if (tp.drop_after_ns > 0 && age >= tp.drop_after_ns) {
            ZEPTO_INFO("Tiering DROP: symbol={} hour={} age={}h",
                      part->key().symbol_id, part->key().hour_epoch,
                      age / 3'600'000'000'000LL);
            pm_.evict_partition(part);
            continue;
        }

        // COLD tier: flush to S3 Parquet if not already uploaded
        if (tp.cold_after_ns > 0 && age >= tp.cold_after_ns) {
            if (part->state() == Partition::State::FLUSHED) {
                // Already on local disk — upload to S3 and reclaim local
                if (s3_sink_ && parquet_writer_) {
                    flush_partition_parquet(*part);
                    ZEPTO_INFO("Tiering COLD: symbol={} hour={} → S3",
                              part->key().symbol_id, part->key().hour_epoch);
                }
            } else if (part->state() == Partition::State::ACTIVE) {
                // Still in memory — seal, flush to disk, then S3
                part->seal();
                part->set_state(Partition::State::FLUSHING);
                writer_.flush_partition(*part);
                if (s3_sink_ && parquet_writer_) {
                    flush_partition_parquet(*part);
                }
                part->set_state(Partition::State::FLUSHED);
                if (config_.reclaim_after_flush) part->reclaim_arena();
                stat_partitions_flushed_.fetch_add(1, std::memory_order_relaxed);
                ZEPTO_INFO("Tiering COLD: symbol={} hour={} → SSD + S3",
                          part->key().symbol_id, part->key().hour_epoch);
            }
            continue;
        }

        // WARM tier: seal and flush to local SSD
        if (tp.warm_after_ns > 0 && age >= tp.warm_after_ns) {
            if (part->state() == Partition::State::ACTIVE) {
                part->seal();
                part->set_state(Partition::State::FLUSHING);
                const size_t bytes = writer_.flush_partition(*part);
                if (bytes > 0) {
                    part->set_state(Partition::State::FLUSHED);
                    if (config_.reclaim_after_flush) part->reclaim_arena();
                    stat_partitions_flushed_.fetch_add(1, std::memory_order_relaxed);
                    stat_bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
                    ZEPTO_INFO("Tiering WARM: symbol={} hour={} → SSD ({}B)",
                              part->key().symbol_id, part->key().hour_epoch, bytes);
                } else {
                    part->set_state(Partition::State::SEALED);
                }
            }
        }
        // else: HOT tier — keep in memory, no action
    }
}

// ============================================================================
// stats: 통계 스냅샷
// ============================================================================
FlushStats FlushManager::stats() const {
    FlushStats s;
    s.partitions_flushed  = stat_partitions_flushed_.load(std::memory_order_relaxed);
    s.total_bytes_written = stat_bytes_written_.load(std::memory_order_relaxed);
    s.flush_triggers      = stat_flush_triggers_.load(std::memory_order_relaxed);
    s.manual_flushes      = stat_manual_flushes_.load(std::memory_order_relaxed);
    s.last_flush_ns       = stat_last_flush_ns_.load(std::memory_order_relaxed);

    // double 비트캐스트 복원
    const uint64_t bits = stat_last_memory_ratio_bits_.load(std::memory_order_relaxed);
    s.last_memory_ratio = std::bit_cast<double>(bits);

    return s;
}

// ============================================================================
// now_ns: 현재 시간 (나노초)
// ============================================================================
int64_t FlushManager::now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace zeptodb::storage

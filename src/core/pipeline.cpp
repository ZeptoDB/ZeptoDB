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
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

static bool append_default_value(ColumnVector& column) {
    switch (column.type()) {
    case ColumnType::INT32:
        return column.append<int32_t>(0);
    case ColumnType::INT64:
    case ColumnType::TIMESTAMP_NS:
        return column.append<int64_t>(0);
    case ColumnType::FLOAT32:
        return column.append<float>(0.0F);
    case ColumnType::FLOAT64:
        return column.append<double>(0.0);
    case ColumnType::SYMBOL:
    case ColumnType::STRING:
        return column.append<uint32_t>(0);
    case ColumnType::BOOL:
        return column.append<uint8_t>(0);
    }
    return false;
}

static bool append_typed_value(ColumnVector& column, const TypedColumnValue& value) {
    if (column.type() != value.type) {
        return false;
    }
    switch (column.type()) {
    case ColumnType::INT32:
        if (value.i64 < std::numeric_limits<int32_t>::min() ||
            value.i64 > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        return column.append<int32_t>(static_cast<int32_t>(value.i64));
    case ColumnType::INT64:
    case ColumnType::TIMESTAMP_NS:
        return column.append<int64_t>(value.i64);
    case ColumnType::FLOAT32:
        return column.append<float>(static_cast<float>(value.f64));
    case ColumnType::FLOAT64:
        return column.append<double>(value.f64);
    case ColumnType::SYMBOL:
    case ColumnType::STRING:
        return column.append<uint32_t>(value.u32);
    case ColumnType::BOOL:
        return column.append<uint8_t>(value.u8 == 0 ? 0 : 1);
    }
    return false;
}

static std::optional<uint64_t> parse_unsigned_component(std::string_view text) {
    if (text.empty()) return std::nullopt;
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

static std::optional<int64_t> parse_signed_component(std::string_view text) {
    if (text.empty()) return std::nullopt;
    int64_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

static std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

static TypedColumnValue recovered_typed_value(
    const ColumnDef& definition,
    const MappedColumn& column,
    size_t row) {
    TypedColumnValue value;
    value.name = definition.name;
    value.type = definition.type;
    switch (definition.type) {
    case ColumnType::INT32:
        value.i64 = column.as_span<int32_t>()[row];
        break;
    case ColumnType::INT64:
    case ColumnType::TIMESTAMP_NS:
        value.i64 = column.as_span<int64_t>()[row];
        break;
    case ColumnType::FLOAT32:
        value.f64 = column.as_span<float>()[row];
        break;
    case ColumnType::FLOAT64:
        value.f64 = column.as_span<double>()[row];
        break;
    case ColumnType::SYMBOL:
    case ColumnType::STRING:
        value.u32 = column.as_span<uint32_t>()[row];
        break;
    case ColumnType::BOOL:
        value.u8 = column.as_span<uint8_t>()[row];
        break;
    }
    return value;
}

static std::optional<SymbolId> recovered_symbol_value(
    const ColumnDef& definition,
    const MappedColumn& column,
    size_t row) {
    uint64_t value = 0;
    switch (definition.type) {
    case ColumnType::INT32: {
        const int32_t raw = column.as_span<int32_t>()[row];
        if (raw < 0) return std::nullopt;
        value = static_cast<uint32_t>(raw);
        break;
    }
    case ColumnType::INT64:
    case ColumnType::TIMESTAMP_NS: {
        const int64_t raw = column.as_span<int64_t>()[row];
        if (raw < 0) return std::nullopt;
        value = static_cast<uint64_t>(raw);
        break;
    }
    case ColumnType::FLOAT32: {
        const float raw = column.as_span<float>()[row];
        if (!std::isfinite(raw) || raw < 0.0F ||
            raw > static_cast<float>(std::numeric_limits<SymbolId>::max())) {
            return std::nullopt;
        }
        value = static_cast<uint64_t>(raw);
        break;
    }
    case ColumnType::FLOAT64: {
        const double raw = column.as_span<double>()[row];
        if (!std::isfinite(raw) || raw < 0.0 ||
            raw > static_cast<double>(std::numeric_limits<SymbolId>::max())) {
            return std::nullopt;
        }
        value = static_cast<uint64_t>(raw);
        break;
    }
    case ColumnType::BOOL: {
        const uint8_t raw = column.as_span<uint8_t>()[row];
        if (raw > 1) return std::nullopt;
        value = raw;
        break;
    }
    case ColumnType::SYMBOL:
    case ColumnType::STRING:
        return std::nullopt;
    }
    if (value > std::numeric_limits<SymbolId>::max()) {
        return std::nullopt;
    }
    return static_cast<SymbolId>(value);
}

// ============================================================================
// 생성자 / 소멸자
// ============================================================================
size_t ZeptoPipeline::resolve_ring_buffer_capacity(size_t configured) {
    constexpr size_t kDefault = ingestion::TickPlant::kDefaultCapacity; // 65536
    constexpr size_t kMin     = 4096ULL;
    constexpr size_t kMax     = 16ULL * 1024 * 1024;  // 16 777 216
    const size_t cap = (configured == 0) ? kDefault : configured;
    const bool pow2 = (cap != 0) && ((cap & (cap - 1)) == 0);
    if (!pow2 || cap < kMin || cap > kMax) {
        throw std::invalid_argument(
            "PipelineConfig::ring_buffer_capacity must be a power of two in "
            "[4096, 16777216] (got " + std::to_string(configured) + ")");
    }
    return cap;
}

ZeptoPipeline::ZeptoPipeline(PipelineConfig config)
    : config_(config)
    , tick_plant_(resolve_ring_buffer_capacity(config.ring_buffer_capacity))
    , partition_mgr_(config.arena_size_per_partition)
{
    if (config_.storage_mode != StorageMode::PURE_IN_MEMORY &&
        config_.hdb_base_path.empty()) {
        throw std::invalid_argument(
            "On-disk and tiered storage require PipelineConfig::hdb_base_path");
    }
    if (config_.storage_mode == StorageMode::TIERED &&
        config_.flush_config.reclaim_after_flush) {
        throw std::invalid_argument(
            "Tiered storage cannot reclaim flushed RDB partitions until "
            "general SQL merges HDB rows");
    }
    if (config_.storage_mode == StorageMode::TIERED &&
        config_.max_memory_bytes > 0) {
        throw std::invalid_argument(
            "Tiered storage cannot evict RDB partitions under a memory limit "
            "until general SQL merges HDB rows");
    }
    if (config_.flush_config.enable_auto_snapshot &&
        config_.flush_config.snapshot_path.empty()) {
        throw std::invalid_argument(
            "Auto-snapshot requires FlushConfig::snapshot_path");
    }
    if (config_.flush_config.enable_auto_snapshot &&
        config_.flush_config.snapshot_interval_ms == 0) {
        throw std::invalid_argument(
            "Auto-snapshot requires a non-zero snapshot interval");
    }
    if (config_.enable_recovery && config_.recovery_snapshot_path.empty()) {
        if (!config_.flush_config.snapshot_path.empty()) {
            config_.recovery_snapshot_path = config_.flush_config.snapshot_path;
        } else {
            throw std::invalid_argument(
                "Recovery requires PipelineConfig::recovery_snapshot_path");
        }
    }

    ZEPTO_INFO("ZeptoPipeline 초기화 (arena={}MB, batch={}, mode={}, ring_capacity={})",
              config.arena_size_per_partition / (1024*1024),
              config.drain_batch_size,
              static_cast<int>(config.storage_mode),
              tick_plant_.capacity());

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
                config_.flush_config,
                [this](int64_t cutoff) {
                    return evict_older_than_ns(cutoff);
                }
            );
            ZEPTO_INFO("FlushManager 생성됨 (Tiered 모드)");
        }

        ZEPTO_INFO("HDB 활성화: path={}", config_.hdb_base_path);
    }

    // Load persisted schema catalog if present (HDB modes only).
    if (config_.storage_mode != StorageMode::PURE_IN_MEMORY &&
        !config_.hdb_base_path.empty()) {
        const std::string schema_path = config_.hdb_base_path + "/_schema.json";
        const auto load_result = schema_registry_.load_from_checked(schema_path);
        if (load_result == storage::SchemaCatalogLoadResult::Loaded) {
            ZEPTO_INFO("SchemaRegistry loaded from {} ({} tables)",
                      schema_path, schema_registry_.table_count());
        } else if (load_result == storage::SchemaCatalogLoadResult::Invalid) {
            throw std::runtime_error(
                "SchemaRegistry catalog is unreadable or invalid: " + schema_path);
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

    try {
        if (config_.enable_recovery && !recovery_completed_) {
            namespace fs = std::filesystem;
            const auto resolution = FlushManager::resolve_snapshot(
                config_.recovery_snapshot_path);
            if (resolution.status == SnapshotResolutionStatus::Invalid) {
                throw std::runtime_error(
                    "Snapshot recovery refused: " + resolution.error);
            }

            size_t recovered_rows = 0;
            size_t recovered_partitions = 0;
            if (resolution.status == SnapshotResolutionStatus::Ready) {
                HDBReader snapshot_reader(resolution.generation_path);
                constexpr int64_t kNsPerHour = 3'600'000'000'000LL;

                auto recover_partition = [&](uint16_t table_id,
                                             SymbolId symbol_id,
                                             int64_t hour_epoch) {
                    if (table_id == 0) {
                        auto timestamp = snapshot_reader.read_column(
                            table_id, symbol_id, hour_epoch, COL_TIMESTAMP);
                        auto price = snapshot_reader.read_column(
                            table_id, symbol_id, hour_epoch, COL_PRICE);
                        auto volume = snapshot_reader.read_column(
                            table_id, symbol_id, hour_epoch, COL_VOLUME);
                        auto msg_type = snapshot_reader.read_column(
                            table_id, symbol_id, hour_epoch, COL_MSG_TYPE);
                        const bool price_type_ok =
                            price.type == ColumnType::INT64 ||
                            price.type == ColumnType::FLOAT64;
                        if (!timestamp.valid() || !price.valid() ||
                            !volume.valid() || !msg_type.valid() ||
                            timestamp.type != ColumnType::TIMESTAMP_NS ||
                            !price_type_ok || volume.type != ColumnType::INT64 ||
                            msg_type.type != ColumnType::INT32 ||
                            price.num_rows != timestamp.num_rows ||
                            volume.num_rows != timestamp.num_rows ||
                            msg_type.num_rows != timestamp.num_rows) {
                            throw std::runtime_error(
                                "legacy snapshot partition has inconsistent columns");
                        }

                        const auto timestamps = timestamp.as_span<int64_t>();
                        const auto volumes = volume.as_span<int64_t>();
                        const auto message_types = msg_type.as_span<int32_t>();
                        for (size_t row = 0; row < timestamp.num_rows; ++row) {
                            if ((timestamps[row] / kNsPerHour) * kNsPerHour !=
                                hour_epoch || message_types[row] < 0 ||
                                message_types[row] >
                                    std::numeric_limits<uint8_t>::max()) {
                                throw std::runtime_error(
                                    "legacy snapshot partition key/value mismatch");
                            }
                            TickMessage message{};
                            message.symbol_id = symbol_id;
                            message.recv_ts = timestamps[row];
                            message.volume = volumes[row];
                            message.msg_type = static_cast<uint8_t>(message_types[row]);
                            if (price.type == ColumnType::FLOAT64) {
                                message.price_is_float = true;
                                message.price_f = price.as_span<double>()[row];
                            } else {
                                message.price = price.as_span<int64_t>()[row];
                            }

                            Partition& target = partition_mgr_.get_or_create(
                                0, symbol_id, message.recv_ts);
                            const size_t before = target.num_rows();
                            store_tick(message);
                            if (target.num_rows() != before + 1) {
                                throw std::runtime_error(
                                    "legacy snapshot row could not be restored");
                            }
                        }
                        if (recovered_rows > std::numeric_limits<size_t>::max() -
                                             timestamp.num_rows) {
                            throw std::runtime_error(
                                "snapshot recovered row count overflows size_t");
                        }
                        ++recovered_partitions;
                        recovered_rows += timestamp.num_rows;
                        return;
                    }

                    const auto schema = schema_registry_.get(table_id);
                    if (!schema || schema->columns.empty()) {
                        throw std::runtime_error(
                            "snapshot references an unknown or empty table schema");
                    }

                    std::vector<MappedColumn> columns;
                    columns.reserve(schema->columns.size());
                    size_t partition_rows = 0;
                    std::optional<size_t> named_symbol;
                    std::optional<size_t> named_timestamp;
                    std::optional<size_t> typed_timestamp;
                    for (size_t index = 0; index < schema->columns.size(); ++index) {
                        const auto& definition = schema->columns[index];
                        if (definition.type == ColumnType::STRING ||
                            definition.type == ColumnType::SYMBOL) {
                            throw std::runtime_error(
                                "snapshot recovery for dictionary columns is not "
                                "available without a durable StringDictionary");
                        }
                        auto column = snapshot_reader.read_column(
                            table_id, symbol_id, hour_epoch, definition.name);
                        if (!column.valid() || column.type != definition.type ||
                            (index != 0 && column.num_rows != partition_rows)) {
                            throw std::runtime_error(
                                "typed snapshot partition has inconsistent columns");
                        }
                        if (index == 0) partition_rows = column.num_rows;

                        const std::string name = lower_ascii(definition.name);
                        if (name == "symbol") {
                            named_symbol = index;
                        }
                        if (name == "timestamp" || name == "timestamp_ns") {
                            if (definition.type != ColumnType::INT64 &&
                                definition.type != ColumnType::TIMESTAMP_NS) {
                                throw std::runtime_error(
                                    "snapshot routing timestamp has an invalid type");
                            }
                            named_timestamp = index;
                        } else if (!typed_timestamp &&
                                   definition.type == ColumnType::TIMESTAMP_NS) {
                            typed_timestamp = index;
                        }
                        columns.push_back(std::move(column));
                    }

                    const std::optional<size_t> route_timestamp =
                        named_timestamp ? named_timestamp : typed_timestamp;
                    for (size_t row = 0; row < partition_rows; ++row) {
                        if (named_symbol) {
                            const auto recovered_symbol = recovered_symbol_value(
                                schema->columns[*named_symbol],
                                columns[*named_symbol], row);
                            if (!recovered_symbol || *recovered_symbol != symbol_id) {
                                throw std::runtime_error(
                                    "typed snapshot symbol routing mismatch");
                            }
                        }
                        TypedRowMessage message;
                        message.table_id = table_id;
                        message.symbol_id = symbol_id;
                        message.timestamp = hour_epoch;
                        message.columns.reserve(columns.size());
                        for (size_t index = 0; index < columns.size(); ++index) {
                            if (schema->columns[index].type == ColumnType::BOOL &&
                                columns[index].as_span<uint8_t>()[row] > 1) {
                                throw std::runtime_error(
                                    "typed snapshot contains an invalid BOOL value");
                            }
                            message.columns.push_back(recovered_typed_value(
                                schema->columns[index], columns[index], row));
                        }
                        if (route_timestamp) {
                            message.timestamp = columns[*route_timestamp]
                                .as_span<int64_t>()[row];
                            if ((message.timestamp / kNsPerHour) * kNsPerHour !=
                                hour_epoch) {
                                throw std::runtime_error(
                                    "typed snapshot partition key/value mismatch");
                            }
                        }
                        if (!ingest_typed_row(std::move(message))) {
                            throw std::runtime_error(
                                "typed snapshot row could not be restored");
                        }
                    }
                    if (recovered_rows > std::numeric_limits<size_t>::max() -
                                         partition_rows) {
                        throw std::runtime_error(
                            "snapshot recovered row count overflows size_t");
                    }
                    ++recovered_partitions;
                    recovered_rows += partition_rows;
                };

                auto validate_partition_files = [&](uint16_t table_id,
                                                      const fs::path& hour_path) {
                    std::unordered_set<std::string> expected_files;
                    if (table_id == 0) {
                        expected_files = {
                            std::string(COL_TIMESTAMP) + ".bin",
                            std::string(COL_PRICE) + ".bin",
                            std::string(COL_VOLUME) + ".bin",
                            std::string(COL_MSG_TYPE) + ".bin",
                        };
                    } else {
                        const auto schema = schema_registry_.get(table_id);
                        if (!schema || schema->columns.empty()) {
                            throw std::runtime_error(
                                "snapshot references an unknown or empty table schema");
                        }
                        for (const auto& definition : schema->columns) {
                            if (!expected_files.emplace(
                                    definition.name + ".bin").second) {
                                throw std::runtime_error(
                                    "snapshot schema contains duplicate column files");
                            }
                        }
                    }

                    std::error_code file_error;
                    for (const auto& file_entry :
                         fs::directory_iterator(hour_path, file_error)) {
                        const bool is_symlink = file_entry.is_symlink(file_error);
                        if (file_error || is_symlink) {
                            throw std::runtime_error(
                                "snapshot partition contains an invalid file entry");
                        }
                        const bool is_regular =
                            file_entry.is_regular_file(file_error);
                        if (file_error || !is_regular) {
                            throw std::runtime_error(
                                "snapshot partition contains an invalid file entry");
                        }
                        if (expected_files.erase(
                                file_entry.path().filename().string()) != 1) {
                            throw std::runtime_error(
                                "snapshot partition contains an unexpected file");
                        }
                    }
                    if (file_error) {
                        throw std::runtime_error(
                            "snapshot partition directory cannot be enumerated");
                    }
                    if (!expected_files.empty()) {
                        throw std::runtime_error(
                            "snapshot partition is missing required column files");
                    }
                };

                auto walk_symbol = [&](const fs::path& symbol_path,
                                       uint16_t table_id) {
                    const std::string symbol_name =
                        symbol_path.filename().string();
                    const auto symbol_value = parse_unsigned_component(symbol_name);
                    if (!symbol_value ||
                        *symbol_value > std::numeric_limits<SymbolId>::max() ||
                        symbol_name != std::to_string(*symbol_value)) {
                        throw std::runtime_error(
                            "snapshot contains an invalid symbol directory");
                    }
                    const SymbolId symbol_id = static_cast<SymbolId>(*symbol_value);

                    std::error_code walk_error;
                    size_t hour_count = 0;
                    for (const auto& hour_entry :
                         fs::directory_iterator(symbol_path, walk_error)) {
                        const bool is_symlink = hour_entry.is_symlink(walk_error);
                        if (walk_error || is_symlink ||
                            !hour_entry.is_directory(walk_error)) {
                            throw std::runtime_error(
                                "snapshot contains an invalid partition entry");
                        }
                        const std::string hour_name =
                            hour_entry.path().filename().string();
                        const auto hour_epoch = parse_signed_component(hour_name);
                        if (!hour_epoch ||
                            hour_name != std::to_string(*hour_epoch) ||
                            *hour_epoch % kNsPerHour != 0) {
                            throw std::runtime_error(
                                "snapshot contains an invalid hour directory");
                        }
                        validate_partition_files(table_id, hour_entry.path());
                        recover_partition(table_id, symbol_id, *hour_epoch);
                        ++hour_count;
                    }
                    if (walk_error) {
                        throw std::runtime_error(
                            "snapshot symbol directory cannot be enumerated");
                    }
                    if (hour_count == 0) {
                        throw std::runtime_error(
                            "snapshot contains an empty symbol directory");
                    }
                };

                std::error_code walk_error;
                for (const auto& entry : fs::directory_iterator(
                         resolution.generation_path, walk_error)) {
                    if (walk_error) {
                        throw std::runtime_error(
                            "snapshot generation cannot be enumerated");
                    }
                    const std::string name = entry.path().filename().string();
                    const bool is_symlink = entry.is_symlink(walk_error);
                    if (walk_error || is_symlink) {
                        throw std::runtime_error(
                            "snapshot generation contains an invalid entry");
                    }
                    if (name == "_COMPLETE" &&
                        entry.is_regular_file(walk_error) && !walk_error) {
                        continue;
                    }
                    if (!entry.is_directory(walk_error) || walk_error) {
                        throw std::runtime_error(
                            "snapshot generation contains an unexpected entry");
                    }

                    if (name.starts_with('t')) {
                        const auto table_value = parse_unsigned_component(
                            std::string_view(name).substr(1));
                        if (!table_value || *table_value == 0 ||
                            *table_value > std::numeric_limits<uint16_t>::max() ||
                            std::string_view(name).substr(1) !=
                                std::to_string(*table_value)) {
                            throw std::runtime_error(
                                "snapshot contains an invalid table directory");
                        }
                        const uint16_t table_id = static_cast<uint16_t>(*table_value);
                        std::error_code table_error;
                        size_t symbol_count = 0;
                        for (const auto& symbol_entry :
                             fs::directory_iterator(entry.path(), table_error)) {
                            const bool symbol_is_symlink =
                                symbol_entry.is_symlink(table_error);
                            if (table_error || symbol_is_symlink ||
                                !symbol_entry.is_directory(table_error)) {
                                throw std::runtime_error(
                                    "snapshot table contains an invalid symbol entry");
                            }
                            walk_symbol(symbol_entry.path(), table_id);
                            ++symbol_count;
                        }
                        if (table_error) {
                            throw std::runtime_error(
                                "snapshot table directory cannot be enumerated");
                        }
                        if (symbol_count == 0) {
                            throw std::runtime_error(
                                "snapshot contains an empty table directory");
                        }
                    } else {
                        walk_symbol(entry.path(), 0);
                    }
                }
                if (walk_error) {
                    throw std::runtime_error(
                        "snapshot generation cannot be enumerated");
                }

                if (recovered_partitions != resolution.partition_count ||
                    recovered_rows != resolution.row_count) {
                    throw std::runtime_error(
                        "snapshot manifest row/partition counts do not match data");
                }
                ZEPTO_INFO("Recovery complete: {} rows from {} partitions ({})",
                           recovered_rows, recovered_partitions,
                           resolution.generation_path);
            } else {
                ZEPTO_INFO("Recovery: no published snapshot at {}",
                           config_.recovery_snapshot_path);
            }
            recovery_completed_ = true;
        }
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }

    // FlushManager 시작 (Tiered 모드)
    if (flush_manager_) {
        flush_manager_->start();
    }

    size_t n_drain = config_.drain_threads;
    if (n_drain == 0) {
        // Auto: at least 2, up to hardware_concurrency()/4.
        const size_t hc = std::thread::hardware_concurrency();
        n_drain = std::max<size_t>(2, hc / 4);
    }
    n_drain = std::max<size_t>(1, n_drain);  // defense-in-depth: never 0
    for (size_t i = 0; i < n_drain; ++i)
        drain_threads_.emplace_back([this]() { drain_loop(); });
    ZEPTO_INFO("ZeptoPipeline 시작 완료 (drain_threads={}, ring_capacity={})",
              n_drain, tick_plant_.capacity());
}

bool ZeptoPipeline::stop() {
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
    bool snapshot_ok = true;
    if (flush_manager_ && !config_.flush_config.snapshot_path.empty()) {
        flush_manager_->snapshot_now();
        snapshot_ok = flush_manager_->last_snapshot_succeeded();
        if (!snapshot_ok) {
            ZEPTO_ERROR("ZeptoPipeline graceful shutdown snapshot failed: {}",
                        config_.flush_config.snapshot_path);
        }
    }
    ZEPTO_INFO("ZeptoPipeline stopped (remaining_flush={}, snapshot_ok={})",
               remaining, snapshot_ok);
    return snapshot_ok;
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

bool ZeptoPipeline::ingest_typed_row(TypedRowMessage row) {
    const int64_t t0 = pipeline_now_ns();
    if (row.table_id == 0 || row.columns.empty()) {
        return false;
    }
    const auto table_schema = schema_registry_.get(row.table_id);
    if (!table_schema) {
        return false;
    }

    std::unordered_map<std::string, TypedColumnValue*> values;
    values.reserve(row.columns.size());
    for (auto& value : row.columns) {
        if (value.name.empty()) {
            return false;
        }
        if (!values.emplace(value.name, &value).second) {
            return false;
        }
        const auto schema_it = std::find_if(
            table_schema->columns.begin(),
            table_schema->columns.end(),
            [&](const ColumnDef& column) { return column.name == value.name; });
        if (schema_it == table_schema->columns.end() || schema_it->type != value.type) {
            return false;
        }
    }
    for (auto& value : row.columns) {
        if (value.has_string_value &&
            (value.type == ColumnType::SYMBOL || value.type == ColumnType::STRING) &&
            !symbol_dict_.ensure_code(value.u32, value.string_value)) {
            return false;
        }
    }

    Partition& partition = partition_mgr_.get_or_create(
        row.table_id, row.symbol_id, row.timestamp);
    auto partition_write = partition.lock_for_write();
    const bool new_partition = partition.columns().empty();

    for (const auto& schema_column : table_schema->columns) {
        if (const auto* column = partition.get_column(schema_column.name)) {
            if (column->type() != schema_column.type) {
                return false;
            }
        }
    }

    const size_t existing_rows = partition.num_rows();
    for (const auto& schema_column : table_schema->columns) {
        if (partition.get_column(schema_column.name)) {
            continue;
        }
        auto& column = partition.add_column(schema_column.name, schema_column.type);
        for (size_t i = 0; i < existing_rows; ++i) {
            if (!append_default_value(column)) {
                return false;
            }
        }
    }

    if (new_partition) {
        std::lock_guard<std::mutex> lk(partition_index_mu_);
        const uint64_t k = (static_cast<uint64_t>(row.table_id) << 32)
                         | static_cast<uint32_t>(row.symbol_id);
        partition_index_[k].push_back(&partition);
        stats_.partitions_created.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<std::pair<ColumnVector*, size_t>> touched;
    touched.reserve(partition.columns().size());
    for (const auto& column_ptr : partition.columns()) {
        ColumnVector& column = *column_ptr;
        const size_t old_size = column.size();
        touched.push_back({&column, old_size});

        const auto value_it = values.find(column.name());
        const bool ok = value_it == values.end()
            ? append_default_value(column)
            : append_typed_value(column, *value_it->second);
        if (!ok) {
            for (auto& [touched_column, size] : touched) {
                touched_column->set_size(size);
            }
            return false;
        }
    }

    stats_.ticks_ingested.fetch_add(1, std::memory_order_relaxed);
    stats_.ticks_stored.fetch_add(1, std::memory_order_relaxed);
    stats_.last_ingest_latency_ns.store(
        pipeline_now_ns() - t0, std::memory_order_relaxed);
    schema_registry_.mark_has_data(table_schema->table_name);
    return true;
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
    auto partition_write = partition.lock_for_write();

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

// ============================================================================
// ZeptoDB: Arrow IPC encoder — Implementation
// ============================================================================
// Single source of truth for Arrow schema + RecordBatch construction from a
// `QueryResultSet`. Used by both the Arrow Flight server (`DoGet`,
// `GetFlightInfo`) and the HTTP `POST /` Arrow content-negotiation path
// (devlog 119).
//
// When `ZEPTO_FLIGHT_ENABLED` is not defined, this TU compiles down to a
// pair of stubs so that `zepto_server` (and `http_server.cpp`) can call
// the public API unconditionally.
// ============================================================================

#include "zeptodb/server/arrow_ipc.h"

#ifdef ZEPTO_FLIGHT_ENABLED

#include <arrow/api.h>
#include <arrow/builder.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/record_batch.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace zeptodb::server {

namespace {

// Convert ZeptoDB ColumnType to Arrow DataType.
std::shared_ptr<arrow::DataType> to_arrow_type(zeptodb::storage::ColumnType ct) {
    using CT = zeptodb::storage::ColumnType;
    switch (ct) {
        case CT::INT64:     return arrow::int64();
        case CT::FLOAT32:   return arrow::float32();
        case CT::FLOAT64:   return arrow::float64();
        case CT::SYMBOL:    return arrow::utf8();   // dict-encoded string
        case CT::STRING:    return arrow::utf8();
        default:            return arrow::int64();
    }
}

int64_t arrow_ingest_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool scaled_to_i64(double value,
                   double scale,
                   const std::string& column,
                   int64_t* out,
                   std::string* error)
{
    const double scaled = value * scale;
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        if (error) *error = "Column '" + column + "' value is out of int64 range";
        return false;
    }
    *out = static_cast<int64_t>(scaled);
    return true;
}

bool array_value_to_i64(const std::shared_ptr<arrow::Array>& array,
                        int64_t row,
                        double scale,
                        const std::string& column,
                        int64_t* out,
                        std::string* error)
{
    if (!array || row < 0 || row >= array->length() || array->IsNull(row)) {
        if (error) *error = "Column '" + column + "' contains null";
        return false;
    }

    switch (array->type_id()) {
        case arrow::Type::INT8:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::Int8Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::INT16:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::Int16Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::INT32:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::Int32Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::INT64:
            if (scale == 1.0) {
                *out = std::static_pointer_cast<arrow::Int64Array>(array)->Value(row);
                return true;
            }
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::Int64Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::UINT8:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::UInt8Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::UINT16:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::UInt16Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::UINT32:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::UINT64:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::FLOAT:
            return scaled_to_i64(
                static_cast<double>(
                    std::static_pointer_cast<arrow::FloatArray>(array)->Value(row)),
                scale, column, out, error);
        case arrow::Type::DOUBLE:
            return scaled_to_i64(
                std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row),
                scale, column, out, error);
        default:
            if (error) {
                *error = "Column '" + column + "' has unsupported numeric type " +
                         array->type()->ToString();
            }
            return false;
    }
}

bool array_value_to_timestamp_ns(const std::shared_ptr<arrow::Array>& array,
                                 int64_t row,
                                 const std::string& column,
                                 int64_t* out,
                                 std::string* error)
{
    if (!array || row < 0 || row >= array->length() || array->IsNull(row)) {
        if (error) *error = "Column '" + column + "' contains null";
        return false;
    }

    if (array->type_id() != arrow::Type::TIMESTAMP) {
        return array_value_to_i64(array, row, 1.0, column, out, error);
    }

    const auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
    int64_t value = ts_array->Value(row);
    const auto ts_type = std::static_pointer_cast<arrow::TimestampType>(array->type());
    int64_t multiplier = 1;
    switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: multiplier = 1'000'000'000LL; break;
        case arrow::TimeUnit::MILLI:  multiplier = 1'000'000LL; break;
        case arrow::TimeUnit::MICRO:  multiplier = 1'000LL; break;
        case arrow::TimeUnit::NANO:   multiplier = 1; break;
    }
    if (multiplier != 1 &&
        (value > std::numeric_limits<int64_t>::max() / multiplier ||
         value < std::numeric_limits<int64_t>::min() / multiplier)) {
        if (error) *error = "Column '" + column + "' timestamp overflows ns";
        return false;
    }
    *out = value * multiplier;
    return true;
}

bool array_value_to_symbol_id(zeptodb::sql::QueryExecutor& executor,
                              const std::shared_ptr<arrow::Array>& array,
                              int64_t row,
                              const std::string& column,
                              zeptodb::SymbolId* out,
                              std::string* error)
{
    if (!array || row < 0 || row >= array->length() || array->IsNull(row)) {
        if (error) *error = "Column '" + column + "' contains null";
        return false;
    }

    if (array->type_id() == arrow::Type::STRING) {
        const auto s = std::static_pointer_cast<arrow::StringArray>(array)->GetString(row);
        *out = executor.intern_symbol_for_ingest(s);
        return true;
    }
    if (array->type_id() == arrow::Type::LARGE_STRING) {
        const auto s =
            std::static_pointer_cast<arrow::LargeStringArray>(array)->GetString(row);
        *out = executor.intern_symbol_for_ingest(s);
        return true;
    }

    int64_t v = 0;
    if (!array_value_to_i64(array, row, 1.0, column, &v, error)) return false;
    if (v < 0 ||
        v > static_cast<int64_t>(std::numeric_limits<zeptodb::SymbolId>::max())) {
        if (error) *error = "Column '" + column + "' symbol id is out of uint32 range";
        return false;
    }
    *out = static_cast<zeptodb::SymbolId>(v);
    return true;
}

int find_column(const std::shared_ptr<arrow::Schema>& schema,
                const std::string& name)
{
    if (!schema || name.empty()) return -1;
    return schema->GetFieldIndex(name);
}

int find_symbol_column(const std::shared_ptr<arrow::Schema>& schema,
                       const std::string& requested)
{
    int idx = find_column(schema, requested);
    if (idx >= 0) return idx;
    if (requested == "sym") return find_column(schema, "symbol");
    if (requested == "symbol") return find_column(schema, "sym");
    return -1;
}

bool decode_record_batch_ticks(zeptodb::sql::QueryExecutor& executor,
                               const std::shared_ptr<arrow::RecordBatch>& batch,
                               const ArrowIpcIngestOptions& options,
                               size_t row_offset,
                               std::vector<zeptodb::ingestion::TickMessage>* out,
                               std::string* error)
{
    const auto schema = batch ? batch->schema() : nullptr;
    const int symbol_idx = find_symbol_column(schema, options.symbol_column);
    const int price_idx = find_column(schema, options.price_column);
    const int volume_idx = find_column(schema, options.volume_column);
    const int timestamp_idx = find_column(schema, options.timestamp_column);
    const int msg_type_idx = find_column(schema, options.msg_type_column);

    if (symbol_idx < 0) {
        if (error) *error = "Missing Arrow column '" + options.symbol_column +
                            "' (alias 'symbol' is accepted for default 'sym')";
        return false;
    }
    if (price_idx < 0) {
        if (error) *error = "Missing Arrow column '" + options.price_column + "'";
        return false;
    }
    if (volume_idx < 0) {
        if (error) *error = "Missing Arrow column '" + options.volume_column + "'";
        return false;
    }

    const auto symbol_col = batch->column(symbol_idx);
    const auto price_col = batch->column(price_idx);
    const auto volume_col = batch->column(volume_idx);
    const auto timestamp_col = timestamp_idx >= 0 ? batch->column(timestamp_idx) : nullptr;
    const auto msg_type_col = msg_type_idx >= 0 ? batch->column(msg_type_idx) : nullptr;

    const int64_t base_ts = arrow_ingest_now_ns() + static_cast<int64_t>(row_offset);
    out->clear();
    out->reserve(static_cast<size_t>(batch->num_rows()));

    for (int64_t row = 0; row < batch->num_rows(); ++row) {
        zeptodb::ingestion::TickMessage msg{};
        if (!array_value_to_symbol_id(executor, symbol_col, row,
                                      schema->field(symbol_idx)->name(),
                                      &msg.symbol_id, error)) {
            return false;
        }
        if (!array_value_to_i64(price_col, row, options.price_scale,
                                schema->field(price_idx)->name(),
                                &msg.price, error)) {
            return false;
        }
        if (!array_value_to_i64(volume_col, row, options.volume_scale,
                                schema->field(volume_idx)->name(),
                                &msg.volume, error)) {
            return false;
        }
        if (timestamp_col) {
            if (!array_value_to_timestamp_ns(timestamp_col, row,
                                             schema->field(timestamp_idx)->name(),
                                             &msg.recv_ts, error)) {
                return false;
            }
        } else {
            msg.recv_ts = base_ts + row;
        }
        if (msg_type_col) {
            int64_t msg_type = 0;
            if (!array_value_to_i64(msg_type_col, row, 1.0,
                                    schema->field(msg_type_idx)->name(),
                                    &msg_type, error)) {
                return false;
            }
            if (msg_type < 0 || msg_type > std::numeric_limits<uint8_t>::max()) {
                if (error) *error = "Column '" + schema->field(msg_type_idx)->name() +
                                    "' msg_type is out of uint8 range";
                return false;
            }
            msg.msg_type = static_cast<uint8_t>(msg_type);
        }
        msg.price_is_float = 0;
        out->push_back(msg);
    }
    return true;
}

} // anonymous namespace

std::shared_ptr<arrow::Schema> build_arrow_schema(
    const zeptodb::sql::QueryResultSet& rs)
{
    using CT = zeptodb::storage::ColumnType;
    arrow::FieldVector fields;
    fields.reserve(rs.column_names.size());
    for (size_t i = 0; i < rs.column_names.size(); ++i) {
        std::shared_ptr<arrow::DataType> type;
        if (i < rs.column_types.size()) {
            auto ct = rs.column_types[i];
            if ((ct == CT::STRING || ct == CT::SYMBOL) && rs.symbol_dict == nullptr) {
                // Defensive: SYMBOL/STRING without dict → expose as int64 codes
                // (unreachable today, see devlog 119 review).
                type = arrow::int64();
            } else {
                type = to_arrow_type(ct);
            }
        } else {
            type = arrow::int64();
        }
        fields.push_back(arrow::field(rs.column_names[i], type));
    }
    return arrow::schema(fields);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> result_to_record_batch(
    const zeptodb::sql::QueryResultSet& rs,
    const std::shared_ptr<arrow::Schema>& schema)
{
    const size_t ncols    = rs.column_names.size();
    const bool   use_typed = !rs.typed_rows.empty();
    const size_t nrows    = use_typed ? rs.typed_rows.size() : rs.rows.size();

    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(ncols);

    for (size_t c = 0; c < ncols; ++c) {
        auto ct = (c < rs.column_types.size())
            ? rs.column_types[c]
            : zeptodb::storage::ColumnType::INT64;
        using CT = zeptodb::storage::ColumnType;

        if ((ct == CT::STRING || ct == CT::SYMBOL) && rs.symbol_dict) {
            arrow::StringBuilder sb;
            ARROW_RETURN_NOT_OK(sb.Reserve(static_cast<int64_t>(nrows)));
            for (size_t r = 0; r < nrows; ++r) {
                int64_t code = use_typed ? rs.typed_rows[r][c].i
                                         : rs.rows[r][c];
                auto name = rs.symbol_dict->lookup(static_cast<int32_t>(code));
                ARROW_RETURN_NOT_OK(sb.Append(name));
            }
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(sb.Finish(&arr));
            arrays.push_back(std::move(arr));
        } else if (ct == CT::FLOAT64 && use_typed) {
            arrow::DoubleBuilder db;
            ARROW_RETURN_NOT_OK(db.Reserve(static_cast<int64_t>(nrows)));
            for (size_t r = 0; r < nrows; ++r)
                ARROW_RETURN_NOT_OK(db.Append(rs.typed_rows[r][c].f));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(db.Finish(&arr));
            arrays.push_back(std::move(arr));
        } else if (ct == CT::FLOAT32 && use_typed) {
            arrow::FloatBuilder fb;
            ARROW_RETURN_NOT_OK(fb.Reserve(static_cast<int64_t>(nrows)));
            for (size_t r = 0; r < nrows; ++r)
                ARROW_RETURN_NOT_OK(fb.Append(static_cast<float>(rs.typed_rows[r][c].f)));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(fb.Finish(&arr));
            arrays.push_back(std::move(arr));
        } else {
            arrow::Int64Builder ib;
            ARROW_RETURN_NOT_OK(ib.Reserve(static_cast<int64_t>(nrows)));
            for (size_t r = 0; r < nrows; ++r) {
                int64_t v = use_typed ? rs.typed_rows[r][c].i : rs.rows[r][c];
                ARROW_RETURN_NOT_OK(ib.Append(v));
            }
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(ib.Finish(&arr));
            arrays.push_back(std::move(arr));
        }
    }

    return arrow::RecordBatch::Make(schema, static_cast<int64_t>(nrows), arrays);
}

bool arrow_ipc_available() noexcept { return true; }

bool encode_result_set_ipc(const zeptodb::sql::QueryResultSet& rs,
                           std::string* out,
                           std::string* err)
{
    if (!out || !err) return false;

    auto schema = build_arrow_schema(rs);
    auto batch_res = result_to_record_batch(rs, schema);
    if (!batch_res.ok()) {
        *err = batch_res.status().ToString();
        return false;
    }
    auto batch = *batch_res;

    auto sink_res = arrow::io::BufferOutputStream::Create();
    if (!sink_res.ok()) {
        *err = sink_res.status().ToString();
        return false;
    }
    auto sink = *sink_res;

    auto writer_res = arrow::ipc::MakeStreamWriter(sink, schema);
    if (!writer_res.ok()) {
        *err = writer_res.status().ToString();
        return false;
    }
    auto writer = *writer_res;

    if (auto st = writer->WriteRecordBatch(*batch); !st.ok()) {
        *err = st.ToString();
        return false;
    }
    if (auto st = writer->Close(); !st.ok()) {
        *err = st.ToString();
        return false;
    }

    auto buf_res = sink->Finish();
    if (!buf_res.ok()) {
        *err = buf_res.status().ToString();
        return false;
    }
    auto buf = *buf_res;
    out->assign(reinterpret_cast<const char*>(buf->data()),
                static_cast<size_t>(buf->size()));
    return true;
}

ArrowIpcIngestResult ingest_arrow_ipc_stream(
    zeptodb::sql::QueryExecutor& executor,
    const std::string& ipc_body,
    const ArrowIpcIngestOptions& options)
{
    ArrowIpcIngestResult result;
    if (ipc_body.empty()) {
        result.error = "Empty Arrow IPC body";
        return result;
    }
    if (!std::isfinite(options.price_scale) ||
        !std::isfinite(options.volume_scale)) {
        result.error = "price_scale and volume_scale must be finite";
        return result;
    }

    auto buffer = arrow::Buffer::FromString(ipc_body);
    auto input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input);
    if (!reader_res.ok()) {
        result.error = reader_res.status().ToString();
        return result;
    }
    auto reader = *reader_res;

    size_t row_offset = 0;
    while (true) {
        auto batch_res = reader->Next();
        if (!batch_res.ok()) {
            result.error = batch_res.status().ToString();
            result.rows = row_offset;
            return result;
        }
        auto batch = *batch_res;
        if (!batch) break;

        std::vector<zeptodb::ingestion::TickMessage> ticks;
        std::string decode_error;
        if (!decode_record_batch_ticks(executor, batch, options, row_offset,
                                       &ticks, &decode_error)) {
            result.error = decode_error;
            result.rows = row_offset;
            return result;
        }

        auto ingest = executor.ingest_tick_batch(options.table_name, std::move(ticks));
        row_offset += ingest.inserted;
        result.failed += ingest.failed;
        if (!ingest.ok()) {
            result.error = ingest.error;
            result.rows = row_offset;
            return result;
        }
    }

    result.ok = true;
    result.rows = row_offset;
    return result;
}

} // namespace zeptodb::server

#else // !ZEPTO_FLIGHT_ENABLED — stubs

namespace zeptodb::server {

bool arrow_ipc_available() noexcept { return false; }

bool encode_result_set_ipc(const zeptodb::sql::QueryResultSet& /*rs*/,
                           std::string* out,
                           std::string* err)
{
    if (!out || !err) return false;
    *err = "Arrow IPC support not compiled in (rebuild with ZEPTO_USE_FLIGHT=ON)";
    return false;
}

ArrowIpcIngestResult ingest_arrow_ipc_stream(
    zeptodb::sql::QueryExecutor& /*executor*/,
    const std::string& /*ipc_body*/,
    const ArrowIpcIngestOptions& /*options*/)
{
    return ArrowIpcIngestResult{
        .ok = false,
        .rows = 0,
        .failed = 0,
        .error = "Arrow IPC support not compiled in (rebuild with ZEPTO_USE_FLIGHT=ON)",
    };
}

} // namespace zeptodb::server

#endif // ZEPTO_FLIGHT_ENABLED

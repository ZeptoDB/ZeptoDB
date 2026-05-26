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
#include <arrow/ipc/writer.h>
#include <arrow/record_batch.h>

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

} // namespace zeptodb::server

#endif // ZEPTO_FLIGHT_ENABLED

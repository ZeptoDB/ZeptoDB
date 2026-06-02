#pragma once
// ============================================================================
// ZeptoDB: Arrow IPC helpers (shared between HTTP and Flight servers)
// ============================================================================
// Encodes a `QueryResultSet` as an Arrow IPC RecordBatchStream and decodes an
// Arrow IPC stream into ZeptoDB tick ingestion rows.
//
// The public API is unconditionally declared so callers (HTTP server,
// Flight server) can compile against the same surface regardless of how
// the build was configured. When the build was made without Arrow support
// (`ZEPTO_USE_FLIGHT=OFF` → no `ZEPTO_FLIGHT_ENABLED`), `arrow_ipc_available()`
// returns `false` and the encode/ingest helpers return explanatory errors.
//
// Used by:
//   - HTTP `POST /` (devlog 119) — Arrow IPC content negotiation
//   - HTTP `POST /insert/arrow`  — Arrow IPC tick ingest
//   - Arrow Flight `DoGet`       — RecordBatch construction (devlog 042)
// ============================================================================

#include "zeptodb/sql/executor.h"
#include <cstddef>
#include <string>

namespace zeptodb::server {

/// True iff this build was compiled with Arrow IPC support.
/// Equivalent to `#ifdef ZEPTO_FLIGHT_ENABLED` at the call site, but
/// queryable at runtime so the public API stays unconditional.
bool arrow_ipc_available() noexcept;

/// Encode a `QueryResultSet` as an Arrow IPC stream (RecordBatchStream format).
///
/// On success: writes the IPC bytes into `*out`, returns `true`.
/// On failure (Arrow error or unsupported build): writes a human-readable
/// message into `*err`, returns `false` and leaves `*out` untouched.
///
/// `out` and `err` must be non-null.
bool encode_result_set_ipc(const zeptodb::sql::QueryResultSet& rs,
                           std::string* out,
                           std::string* err);

/// Column mapping and scaling for Arrow IPC tick ingest.
///
/// Required: symbol, price, volume. The symbol column may be integer-typed or
/// utf8; utf8 values are interned through QueryExecutor. `timestamp_column` is
/// optional: when absent, ZeptoDB assigns monotonically increasing ns stamps
/// starting at ingest time. Arrow timestamp arrays are converted to ns.
struct ArrowIpcIngestOptions {
    std::string table_name;
    std::string symbol_column = "sym";
    std::string price_column = "price";
    std::string volume_column = "volume";
    std::string timestamp_column = "timestamp";
    std::string msg_type_column = "msg_type";
    double price_scale = 1.0;
    double volume_scale = 1.0;
};

/// Result of a decoded Arrow IPC ingest request.
struct ArrowIpcIngestResult {
    bool ok = false;
    size_t rows = 0;
    size_t failed = 0;
    std::string error;
};

/// Decode an Arrow IPC RecordBatchStream and ingest its rows.
///
/// On success: returns `{ok=true, rows=N}`. On malformed IPC, unsupported
/// column types, missing required columns, unknown table, or route failure:
/// returns `{ok=false, error=...}`.
ArrowIpcIngestResult ingest_arrow_ipc_stream(
    zeptodb::sql::QueryExecutor& executor,
    const std::string& ipc_body,
    const ArrowIpcIngestOptions& options);

} // namespace zeptodb::server

// ----------------------------------------------------------------------------
// Arrow-typed helpers (only declared when Arrow is available at compile time).
// Used by `src/server/flight_server.cpp` to share schema/RecordBatch builders
// with the HTTP IPC encoder. NOT part of the cross-build public surface.
// ----------------------------------------------------------------------------
#ifdef ZEPTO_FLIGHT_ENABLED
#include <arrow/api.h>
#include <arrow/record_batch.h>
#include <memory>

namespace zeptodb::server {

/// Build an Arrow schema from a `QueryResultSet`'s column metadata.
std::shared_ptr<arrow::Schema> build_arrow_schema(
    const zeptodb::sql::QueryResultSet& rs);

/// Build a single Arrow `RecordBatch` from a `QueryResultSet`.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> result_to_record_batch(
    const zeptodb::sql::QueryResultSet& rs,
    const std::shared_ptr<arrow::Schema>& schema);

} // namespace zeptodb::server
#endif // ZEPTO_FLIGHT_ENABLED

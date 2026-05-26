#pragma once
// ============================================================================
// ZeptoDB: Arrow IPC encoder (shared between HTTP and Flight servers)
// ============================================================================
// Encodes a `QueryResultSet` as an Arrow IPC RecordBatchStream.
//
// The public API is unconditionally declared so callers (HTTP server,
// Flight server) can compile against the same surface regardless of how
// the build was configured. When the build was made without Arrow support
// (`ZEPTO_USE_FLIGHT=OFF` → no `ZEPTO_FLIGHT_ENABLED`), `arrow_ipc_available()`
// returns `false` and `encode_result_set_ipc` always returns `false` with
// an explanatory message in `*err`.
//
// Used by:
//   - HTTP `POST /` (devlog 119) — Arrow IPC content negotiation
//   - Arrow Flight `DoGet`       — RecordBatch construction (devlog 042)
// ============================================================================

#include "zeptodb/sql/executor.h"
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

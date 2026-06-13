#pragma once
// ============================================================================
// ZeptoDB: MessagePack columnar ingest helpers
// ============================================================================
// Decodes a small, dependency-free MessagePack columnar payload into ZeptoDB
// tick rows. The supported wire shape is a top-level map where each key is a
// column name and each value is an array. Required columns are symbol, price,
// and volume; timestamp and msg_type are optional.
// ============================================================================

#include "zeptodb/sql/executor.h"

#include <cstddef>
#include <string>

namespace zeptodb::server {

/// Column mapping and scaling for MessagePack tick ingest.
struct MsgpackIngestOptions {
    std::string table_name;
    std::string symbol_column = "sym";
    std::string price_column = "price";
    std::string volume_column = "volume";
    std::string timestamp_column = "timestamp";
    std::string msg_type_column = "msg_type";
    double price_scale = 1.0;
    double volume_scale = 1.0;
};

/// Result of a decoded MessagePack ingest request.
struct MsgpackIngestResult {
    bool ok = false;
    size_t rows = 0;
    size_t failed = 0;
    std::string error;
};

/// Decode a MessagePack map-of-column-arrays and ingest its rows.
///
/// Supported scalar values:
/// - symbols: string, signed integer, unsigned integer
/// - price/volume/timestamp/msg_type: signed/unsigned integer or float
/// - nil in optional timestamp/msg_type means "use default"; nil in required
///   columns is rejected.
MsgpackIngestResult ingest_msgpack_columns(
    zeptodb::sql::QueryExecutor& executor,
    const std::string& body,
    const MsgpackIngestOptions& options);

} // namespace zeptodb::server

#pragma once
// ============================================================================
// Phase C-3: RPC Protocol — wire format and QueryResultSet serialization
// ============================================================================
// Binary layout (all little-endian):
//
//   RpcHeader (24 bytes):
//     magic:       uint32  = 0x41504558 ('APEX')
//     type:        uint32  (SQL_QUERY=1, SQL_RESULT=2, PING=3, PONG=4)
//     request_id:  uint32
//     payload_len: uint32
//     epoch:       uint64  (fencing token — 0 = no fencing)
//
//   SQL_QUERY payload:
//     raw SQL string (payload_len bytes, no NUL)
//
//   SQL_RESULT payload:
//     error_len:  uint32
//     error:      bytes[error_len]
//     col_count:  uint32
//     per-column: name_len(uint32) + name + type(uint8)
//     row_count:  uint32
//     per-row:    col_count * int64 (all values packed)
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"
#include "zeptodb/sql/executor.h"
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace zeptodb::cluster {

// ============================================================================
// RpcType
// ============================================================================
enum class RpcType : uint32_t {
    SQL_QUERY   = 1,
    SQL_RESULT  = 2,
    PING        = 3,
    PONG        = 4,
    TICK_INGEST = 5,  // TickMessage → remote node's local pipeline
    TICK_ACK    = 6,  // 1-byte status response (0x01 = accepted)
    WAL_REPLICATE = 7,  // Batch of TickMessages for WAL replication
    WAL_ACK       = 8,  // 1-byte ack (0x01 = all applied)
    STATS_REQUEST = 9,  // Request PipelineStats + MetricsHistory from node
    STATS_RESULT  = 10, // Response with node stats JSON
    METRICS_REQUEST = 11, // Request metrics history (payload: since_ms:i64 + limit:u32)
    METRICS_RESULT  = 12, // Response with metrics history JSON array
};

// ============================================================================
// RpcHeader — fixed 24-byte wire header
// ============================================================================
#pragma pack(push, 1)
struct RpcHeader {
    uint32_t magic       = 0x41504558u;  // 'APEX'
    uint32_t type        = 0;
    uint32_t request_id  = 0;
    uint32_t payload_len = 0;
    uint64_t epoch       = 0;            // fencing token (0 = no fencing)
};
#pragma pack(pop)

static_assert(sizeof(RpcHeader) == 24, "RpcHeader must be 24 bytes");

// ============================================================================
// Write helpers (append to byte vector)
// ============================================================================

inline void proto_write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >>  8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

inline void proto_write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

inline void proto_write_i64(std::vector<uint8_t>& buf, int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    buf.push_back(static_cast<uint8_t>(u));
    buf.push_back(static_cast<uint8_t>(u >>  8));
    buf.push_back(static_cast<uint8_t>(u >> 16));
    buf.push_back(static_cast<uint8_t>(u >> 24));
    buf.push_back(static_cast<uint8_t>(u >> 32));
    buf.push_back(static_cast<uint8_t>(u >> 40));
    buf.push_back(static_cast<uint8_t>(u >> 48));
    buf.push_back(static_cast<uint8_t>(u >> 56));
}

inline void proto_write_str(std::vector<uint8_t>& buf, const std::string& s) {
    proto_write_u32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// ============================================================================
// Read helpers (from raw pointer, returns updated pointer)
// ============================================================================

inline uint32_t proto_read_u32(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) <<  8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

inline int64_t proto_read_i64(const uint8_t* p) {
    uint64_t u =  static_cast<uint64_t>(p[0])
               | (static_cast<uint64_t>(p[1]) <<  8)
               | (static_cast<uint64_t>(p[2]) << 16)
               | (static_cast<uint64_t>(p[3]) << 24)
               | (static_cast<uint64_t>(p[4]) << 32)
               | (static_cast<uint64_t>(p[5]) << 40)
               | (static_cast<uint64_t>(p[6]) << 48)
               | (static_cast<uint64_t>(p[7]) << 56);
    int64_t v;
    std::memcpy(&v, &u, 8);
    return v;
}

// ============================================================================
// Serialize QueryResultSet → SQL_RESULT payload bytes
// ============================================================================
inline std::vector<uint8_t> serialize_result(const zeptodb::sql::QueryResultSet& r) {
    std::vector<uint8_t> buf;
    buf.reserve(64 + r.rows.size() * (r.column_names.size() + 1) * 8);

    proto_write_str(buf, r.error);

    const uint32_t ncols = static_cast<uint32_t>(r.column_names.size());
    proto_write_u32(buf, ncols);

    for (uint32_t i = 0; i < ncols; ++i) {
        proto_write_str(buf, r.column_names[i]);
        uint8_t col_type = (r.column_types.size() > i)
            ? static_cast<uint8_t>(r.column_types[i])
            : 1u;  // INT64 default
        proto_write_u8(buf, col_type);
    }

    proto_write_u32(buf, static_cast<uint32_t>(r.rows.size()));
    for (const auto& row : r.rows) {
        for (int64_t v : row) {
            proto_write_i64(buf, v);
        }
    }

    return buf;
}

// ============================================================================
// Deserialize SQL_RESULT payload → QueryResultSet
// ============================================================================
inline zeptodb::sql::QueryResultSet deserialize_result(const uint8_t* data, size_t len) {
    zeptodb::sql::QueryResultSet r;
    const uint8_t* p   = data;
    const uint8_t* end = data + len;

    auto need = [&](size_t n) -> bool { return (p + n) <= end; };

    // Error string
    if (!need(4)) { r.error = "proto: truncated(error_len)"; return r; }
    uint32_t err_len = proto_read_u32(p); p += 4;
    if (!need(err_len)) { r.error = "proto: truncated(error)"; return r; }
    r.error.assign(reinterpret_cast<const char*>(p), err_len);
    p += err_len;

    // Column metadata
    if (!need(4)) { r.error = "proto: truncated(col_count)"; return r; }
    uint32_t ncols = proto_read_u32(p); p += 4;
    r.column_names.resize(ncols);
    r.column_types.resize(ncols);

    for (uint32_t i = 0; i < ncols; ++i) {
        if (!need(4)) { r.error = "proto: truncated(name_len)"; return r; }
        uint32_t nlen = proto_read_u32(p); p += 4;
        if (!need(nlen + 1)) { r.error = "proto: truncated(name)"; return r; }
        r.column_names[i].assign(reinterpret_cast<const char*>(p), nlen);
        p += nlen;
        r.column_types[i] = static_cast<zeptodb::storage::ColumnType>(*p++);
    }

    // Rows
    if (!need(4)) { r.error = "proto: truncated(row_count)"; return r; }
    uint32_t nrows = proto_read_u32(p); p += 4;
    r.rows.resize(nrows, std::vector<int64_t>(ncols, 0));

    for (uint32_t ri = 0; ri < nrows; ++ri) {
        for (uint32_t ci = 0; ci < ncols; ++ci) {
            if (!need(8)) { r.error = "proto: truncated(value)"; return r; }
            r.rows[ri][ci] = proto_read_i64(p);
            p += 8;
        }
    }

    return r;
}

// ============================================================================
// Serialize / Deserialize TickMessage (raw 64-byte POD — zero-copy)
// ============================================================================

inline std::vector<uint8_t> serialize_tick(
    const zeptodb::ingestion::TickMessage& msg)
{
    std::vector<uint8_t> buf(sizeof(msg));
    std::memcpy(buf.data(), &msg, sizeof(msg));
    return buf;
}

/// @return true on success; out is valid only when true.
inline bool deserialize_tick(const uint8_t* data, size_t len,
                              zeptodb::ingestion::TickMessage& out)
{
    if (len < sizeof(zeptodb::ingestion::TickMessage)) return false;
    std::memcpy(&out, data, sizeof(out));
    return true;
}

// ============================================================================
// Serialize / Deserialize WAL batch (vector of TickMessages)
// Layout: uint32 count + count * sizeof(TickMessage)
// ============================================================================

inline std::vector<uint8_t> serialize_wal_batch(
    const std::vector<zeptodb::ingestion::TickMessage>& msgs)
{
    const uint32_t count = static_cast<uint32_t>(msgs.size());
    std::vector<uint8_t> buf(4 + count * sizeof(zeptodb::ingestion::TickMessage));
    std::memcpy(buf.data(), &count, 4);
    if (count > 0)
        std::memcpy(buf.data() + 4, msgs.data(),
                    count * sizeof(zeptodb::ingestion::TickMessage));
    return buf;
}

inline bool deserialize_wal_batch(const uint8_t* data, size_t len,
                                   std::vector<zeptodb::ingestion::TickMessage>& out)
{
    if (len < 4) return false;
    uint32_t count;
    std::memcpy(&count, data, 4);
    size_t need = 4 + static_cast<size_t>(count) * sizeof(zeptodb::ingestion::TickMessage);
    if (len < need) return false;
    out.resize(count);
    if (count > 0)
        std::memcpy(out.data(), data + 4,
                    count * sizeof(zeptodb::ingestion::TickMessage));
    return true;
}

// ============================================================================
// Serialize / Deserialize METRICS_REQUEST payload
// Layout: int64 since_ms + uint32 limit
// ============================================================================

inline std::vector<uint8_t> serialize_metrics_request(int64_t since_ms, uint32_t limit) {
    std::vector<uint8_t> buf;
    buf.reserve(12);
    proto_write_i64(buf, since_ms);
    proto_write_u32(buf, limit);
    return buf;
}

inline bool deserialize_metrics_request(const uint8_t* data, size_t len,
                                         int64_t& since_ms, uint32_t& limit) {
    if (len < 12) return false;
    since_ms = proto_read_i64(data);
    limit    = proto_read_u32(data + 8);
    return true;
}

} // namespace zeptodb::cluster

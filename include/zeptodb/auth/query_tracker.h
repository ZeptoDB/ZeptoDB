#pragma once
// ============================================================================
// ZeptoDB: Query Tracker
// ============================================================================
// Tracks active queries for the admin API (/admin/queries).
// Associates each query with a CancellationToken to support kill.
//
// Thread safety: shared_mutex on the registry map.
// ============================================================================
#include "zeptodb/auth/cancellation_token.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <cstdint>

namespace zeptodb::auth {

struct ActiveQueryInfo {
    std::string                        query_id;
    std::string                        subject;      // who submitted
    std::string                        sql_preview;  // first 200 chars
    int64_t                            started_at_ns;
    std::shared_ptr<CancellationToken> token;
};

class QueryTracker {
public:
    static constexpr size_t SQL_PREVIEW_LEN = 200;

    // Register a new query. Returns the assigned query_id.
    std::string register_query(const std::string& subject,
                               const std::string& sql,
                               std::shared_ptr<CancellationToken> token);

    // Mark a query as complete (remove from active set).
    // Idempotent — safe to call even if already removed.
    void complete(const std::string& query_id);

    // Cancel a query by id. Returns false if not found.
    bool cancel(const std::string& query_id);

    // Snapshot of active queries at this instant.
    std::vector<ActiveQueryInfo> list() const;

    // Active query count.
    size_t active_count() const;

private:
    mutable std::shared_mutex                                mu_;
    std::unordered_map<std::string, ActiveQueryInfo>         active_;

    static std::string generate_id();
    static int64_t     now_ns();
};

} // namespace zeptodb::auth

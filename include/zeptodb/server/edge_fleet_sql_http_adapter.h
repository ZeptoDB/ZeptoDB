#pragma once
// ============================================================================
// ZeptoDB: SQL/HTTP-backed edge/fleet connector adapter
// ============================================================================

#include "zeptodb/feeds/edge_fleet_connector_runtime.h"
#include "zeptodb/sql/executor.h"

#include <cstddef>
#include <string>

namespace zeptodb::server {

/// SQL/HTTP source and sink contract for the experimental edge/fleet connector.
///
/// Thread-safety: value type. Hooks created from this config are intended for
/// the server-managed edge/fleet worker. Empty URLs use the server's local
/// QueryExecutor; non-empty URLs execute SQL through ZeptoDB HTTP `POST /`.
/// Identifiers are validated before SQL is emitted; string values are always
/// SQL-literal escaped.
struct EdgeFleetSqlHttpAdapterConfig {
    zeptodb::feeds::EdgeFleetConnectorRuntimeConfig runtime;

    std::string edge_sql_url;
    std::string fleet_sql_url;

    std::string fleet_inbox_table = "physical_ai_fleet_feed_inbox_016";
    std::string fleet_decision_table = "physical_ai_fleet_edge_decisions_016";
    std::string fleet_retrieval_table = "physical_ai_fleet_retrieval_016";
    std::string fleet_suppression_table = "physical_ai_fleet_suppressions_016";
    std::string fleet_telemetry_table = "physical_ai_fleet_feed_telemetry_016";

    /// Maximum edge outbox rows fetched per SQL load. The connector pass is
    /// additionally bounded by `runtime.feed.batch_limit` and
    /// `runtime.feed.max_inflight`.
    size_t outbox_query_limit = 128;

    /// Maximum total decoded SQL cell bytes accepted from one outbox load.
    size_t max_outbox_bytes = 1024 * 1024;

    /// Record one fleet telemetry row after each successful worker pass.
    bool record_pass_telemetry = true;
};

/// Validate adapter URLs, table identifiers, and numeric limits.
[[nodiscard]] bool validateEdgeFleetSqlHttpAdapterConfig(
    const EdgeFleetSqlHttpAdapterConfig& config,
    std::string* error = nullptr);

/// Create the default local SQL contract tables when they are missing.
///
/// This helper only bootstraps local QueryExecutor-backed tables. Remote HTTP
/// endpoints should be migrated separately by the operator.
[[nodiscard]] bool ensureEdgeFleetSqlHttpTables(
    zeptodb::sql::QueryExecutor& executor,
    const EdgeFleetSqlHttpAdapterConfig& config,
    std::string* error = nullptr);

/// Build runtime hooks backed by ZeptoDB SQL tables or HTTP SQL endpoints.
///
/// The loader reads edge outbox rows from `runtime.edge_outbox_table`, the sink
/// materializes fleet inbox/final/ACK rows, and the optional pass observer
/// writes bounded worker telemetry.
[[nodiscard]] zeptodb::feeds::EdgeFleetConnectorRuntimeHooks
makeEdgeFleetSqlHttpRuntimeHooks(
    zeptodb::sql::QueryExecutor& executor,
    EdgeFleetSqlHttpAdapterConfig config);

} // namespace zeptodb::server

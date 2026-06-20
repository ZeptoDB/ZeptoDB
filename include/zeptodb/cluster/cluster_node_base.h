#pragma once
// ============================================================================
// ZeptoDB: ClusterNodeBase — non-template cluster routing interface
// ============================================================================
// Non-template base so non-cluster headers (sql/executor.h, python bindings)
// can hold a routing-aware pointer without pulling in the templated
// ClusterNode<Transport>. See devlog 103.
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"

namespace zeptodb::core {
struct TypedRowMessage;
}  // namespace zeptodb::core

namespace zeptodb::cluster {

class ClusterNodeBase {
public:
    virtual ~ClusterNodeBase() = default;

    /// Route a tick to its partition owner (local or remote via RPC).
    /// Returns true on success.
    virtual bool ingest_tick(zeptodb::ingestion::TickMessage msg) = 0;

    /// Route a schema-aware typed row to its partition owner.
    /// Default false keeps older cluster adapters source-compatible.
    virtual bool ingest_typed_row(const zeptodb::core::TypedRowMessage& row) {
        (void)row;
        return false;
    }
};

}  // namespace zeptodb::cluster

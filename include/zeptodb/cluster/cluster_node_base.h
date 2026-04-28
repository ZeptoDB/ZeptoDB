#pragma once
// ============================================================================
// ZeptoDB: ClusterNodeBase — non-template cluster routing interface
// ============================================================================
// Non-template base so non-cluster headers (sql/executor.h, python bindings)
// can hold a routing-aware pointer without pulling in the templated
// ClusterNode<Transport>. See devlog 103.
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"

namespace zeptodb::cluster {

class ClusterNodeBase {
public:
    virtual ~ClusterNodeBase() = default;

    /// Route a tick to its partition owner (local or remote via RPC).
    /// Returns true on success.
    virtual bool ingest_tick(zeptodb::ingestion::TickMessage msg) = 0;
};

}  // namespace zeptodb::cluster

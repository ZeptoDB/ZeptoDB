#pragma once
// ============================================================================
// ZeptoDB: CoordinatorRoutingAdapter — minimal ClusterNodeBase for the HTTP
// server binary. See BACKLOG P8-I3-wire and devlog 111.
// ============================================================================
// Adapts an existing (PartitionRouter + local pipeline + peer RPC client
// pool) tuple to the ClusterNodeBase interface so that
// QueryExecutor::set_cluster_node() can route HTTP/SQL INSERTs across pods
// without requiring the full templated ClusterNode<Transport>.
//
// Lifetime contract: all four pointer-held resources (router, router_mu,
// local pipeline, remote client map) must outlive this adapter. The caller
// owns every referenced object; this class owns nothing.
// ============================================================================

#include "zeptodb/cluster/cluster_node_base.h"
#include "zeptodb/cluster/partition_router.h"
#include "zeptodb/cluster/rpc_client_base.h"
#include "zeptodb/common/types.h"
#include "zeptodb/core/pipeline.h"

#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace zeptodb::cluster {

class CoordinatorRoutingAdapter : public ClusterNodeBase {
public:
    using RpcClientMap =
        std::unordered_map<NodeId, std::shared_ptr<RpcClientBase>>;

    CoordinatorRoutingAdapter(PartitionRouter*               router,
                              std::shared_mutex*             router_mu,
                              zeptodb::core::ZeptoPipeline*  local_pipeline,
                              NodeId                         self_id,
                              const RpcClientMap*            remote_clients) noexcept
        : router_(router)
        , router_mu_(router_mu)
        , local_(local_pipeline)
        , self_id_(self_id)
        , remote_(remote_clients) {}

    /// Route a tick to its owner: local pipeline if self, else the matching
    /// RPC client. Returns false if the owner is unknown (stale ring view).
    bool ingest_tick(zeptodb::ingestion::TickMessage msg) override {
        NodeId owner;
        {
            std::shared_lock<std::shared_mutex> lk(*router_mu_);
            owner = router_->route(msg.table_id, msg.symbol_id);
        }
        if (owner == self_id_) {
            return local_->ingest_tick(msg);
        }
        auto it = remote_->find(owner);
        if (it == remote_->end()) {
            return false;  // stale ring; caller may increment a drop counter
        }
        return it->second->ingest_tick(msg);
    }

private:
    PartitionRouter*              router_;
    std::shared_mutex*            router_mu_;
    zeptodb::core::ZeptoPipeline* local_;
    NodeId                        self_id_;
    const RpcClientMap*           remote_;
};

}  // namespace zeptodb::cluster

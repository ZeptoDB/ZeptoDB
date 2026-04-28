#pragma once
// ============================================================================
// ZeptoDB: RpcClientBase — non-template remote-tick-ingest interface
// ============================================================================
// Abstract base so connectors (OpcUaConsumer, MqttConsumer, KafkaConsumer)
// can hold `shared_ptr<RpcClientBase>` remotes without coupling to the
// concrete TcpRpcClient (which requires a live server for unit tests).
// Unit tests substitute lightweight counting stubs. Mirrors the
// ClusterNodeBase pattern (devlog 103). BACKLOG P9 #2s, devlog 110.
// ============================================================================

#include "zeptodb/ingestion/tick_plant.h"

namespace zeptodb::cluster {

class RpcClientBase {
public:
    virtual ~RpcClientBase() = default;

    /// Send a tick to the remote node. Returns true on success.
    virtual bool ingest_tick(const zeptodb::ingestion::TickMessage& msg) = 0;
};

}  // namespace zeptodb::cluster

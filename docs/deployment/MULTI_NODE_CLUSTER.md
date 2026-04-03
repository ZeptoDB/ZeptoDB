# Multi-Node Cluster Guide

ZeptoDB multi-node cluster configuration and operations guide.

---

## Architecture

```
┌─────────────────────┐       TCP RPC        ┌──────────────────┐
│  Coordinator (HTTP)  │◄───────────────────►│  Data Node 1     │
│  zepto_http_server   │                      │  zepto_data_node │
│  port 8123           │       TCP RPC        │  port 9001       │
│  Web UI + API        │◄───────────────────►├──────────────────┤
│  node_id=0           │                      │  Data Node 2     │
│                      │                      │  zepto_data_node │
│                      │                      │  port 9002       │
└─────────────────────┘                      └──────────────────┘
```

- **Coordinator** (`zepto_http_server`): Provides HTTP API + Web UI, distributes queries via scatter-gather
- **Data Node** (`zepto_data_node`): Executes queries via TCP RPC, stores data

---

## Binaries

### zepto_http_server (Coordinator)

```
./zepto_http_server [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port`, `-p` | 8123 | HTTP listening port |
| `--ticks`, `-n` | 10000 | Number of sample ticks to generate at startup |
| `--no-auth` | false | Disable authentication |
| `--node-id` | 0 | Cluster ID for this node |
| `--add-node id:host:port` | — | Register a remote data node (can be used multiple times) |
| `--log-level` | info | Log level (debug/info/warn/error) |

The Coordinator is always active. If there are no data nodes, it operates in standalone mode.
When nodes are added via `--add-node` or `POST /admin/nodes`, it automatically switches to cluster mode.

### zepto_data_node (Data Node)

```
./zepto_data_node <port> [num_ticks] [options]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `port` | (required) | TCP RPC listening port |
| `num_ticks` | 0 | Number of sample ticks to generate at startup |
| `--node-id` | port number | Node ID within the cluster |
| `--symbol` | 1 | symbol_id for sample data |
| `--coordinator host:port` | — | Auto-register with the Coordinator |
| `--api-key` | — | Admin API key to use for registration |
| `--advertise-host` | localhost | Host address to advertise to the Coordinator |

---

## Quick Start

### 1. Build

```bash
cd build
ninja zepto_http_server zepto_data_node
```

### 2. Method A: Start Coordinator first → Data Nodes auto-register

```bash
# Terminal 1 — Coordinator
./zepto_http_server --port 8123
# The admin API key will be printed → copy it

# Terminal 2 — Data Node (auto-register)
./zepto_data_node 9001 --coordinator localhost:8123 --api-key $ADMIN_KEY

# Terminal 3 — Data Node (auto-register)
./zepto_data_node 9002 --coordinator localhost:8123 --api-key $ADMIN_KEY
```

### 3. Method B: Specify all nodes at startup

```bash
# Terminals 1~2 — Start Data Nodes first
./zepto_data_node 9001
./zepto_data_node 9002

# Terminal 3 — Coordinator (connect to nodes)
./zepto_http_server --port 8123 \
  --add-node 9001:localhost:9001 \
  --add-node 9002:localhost:9002
```

### 4. Verify via Web UI

Open `http://localhost:3000/cluster` in a browser → 3 nodes should be displayed (coordinator + 2 data nodes)

---

## Runtime Node Management

Even after the cluster has started, you can dynamically add/remove nodes via the REST API.

### Add a node

```bash
# Start a new data node
./zepto_data_node 9003

# Register with the Coordinator
curl -X POST http://localhost:8123/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"id":9003,"host":"localhost","port":9003}'
```

### Remove a node

```bash
curl -X DELETE http://localhost:8123/admin/nodes/9003 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

### List nodes

```bash
curl http://localhost:8123/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY"
```

Example response:
```json
{
  "nodes": [
    {"id": 0, "host": "localhost", "port": 8123, "state": "ACTIVE", "ticks_ingested": 10000, "ticks_stored": 10000, "queries_executed": 5},
    {"id": 9001, "host": "localhost", "port": 9001, "state": "ACTIVE", "ticks_ingested": 0, "ticks_stored": 0, "queries_executed": 0},
    {"id": 9002, "host": "localhost", "port": 9002, "state": "ACTIVE", "ticks_ingested": 0, "ticks_stored": 0, "queries_executed": 0}
  ]
}
```

### Check cluster status

```bash
curl http://localhost:8123/admin/cluster \
  -H "Authorization: Bearer $ADMIN_KEY"
```

Example response:
```json
{
  "mode": "cluster",
  "node_count": 3,
  "ticks_ingested": 10000,
  "ticks_stored": 10000,
  "queries_executed": 5
}
```

---

## Examples

### Single Node (Standalone)

```bash
./zepto_http_server --port 8123
```

The cluster tab shows 1 node. `mode: standalone`. When a data node is added, it automatically switches to `mode: cluster`.

### 2-Node Cluster

```bash
# Terminal 1
./zepto_data_node 9001

# Terminal 2
./zepto_http_server --port 8123 --add-node 9001:localhost:9001
```

### 4-Node Cluster

```bash
# Terminals 1~3: data nodes
./zepto_data_node 9001
./zepto_data_node 9002
./zepto_data_node 9003

# Terminal 4: coordinator
./zepto_http_server --port 8123 \
  --add-node 9001:localhost:9001 \
  --add-node 9002:localhost:9002 \
  --add-node 9003:localhost:9003
```

### Remote Host

```bash
# Coordinator (10.0.1.1)
./zepto_http_server --port 8123

# Machine A (10.0.1.2) — auto-register
./zepto_data_node 9001 \
  --coordinator 10.0.1.1:8123 \
  --api-key $ADMIN_KEY \
  --advertise-host 10.0.1.2

# Machine B (10.0.1.3) — auto-register
./zepto_data_node 9001 \
  --coordinator 10.0.1.1:8123 \
  --api-key $ADMIN_KEY \
  --advertise-host 10.0.1.3
```

---

## Web UI Cluster Tab

Information available on the `/cluster` page:

| Section | Description |
|---------|-------------|
| Summary Cards | Mode (standalone/cluster), number of active nodes, total ticks, total queries |
| Node Status Table | Per-node ID, Host, Port, State, Ticks, Queries |
| Ticks Stored Bar Chart | Bar chart of ticks stored per node |
| Ingestion History | Per-node time-series ingestion trend |
| Queries History | Per-node time-series query execution trend |
| Latency History | Per-node ingest latency trend |

Node state colors:
- 🟢 ACTIVE — Normal
- 🟡 SUSPECT — Response delayed
- 🔴 DEAD — Unreachable
- 🔵 JOINING — Joining the cluster
- ⚪ LEAVING — Leaving the cluster

---

## High Availability (HA)

Active-Standby architecture with automatic failover when the coordinator fails.

```
Coordinator (ACTIVE)  ←── ping (500ms) ──  Coordinator (STANDBY)
  port 8123, rpc 9100                        port 8124, rpc 9101
     │                                            │
     ├── Data Node 1 (9001)                       │
     └── Data Node 2 (9002)                       │
                                                  │
  If ACTIVE dies → after 2 seconds STANDBY is promoted to ACTIVE
                   Node list is automatically re-registered
```

### HA Options

| Option | Description |
|--------|-------------|
| `--ha active\|standby` | HA role |
| `--peer host:port` | RPC address of the peer coordinator |
| `--rpc-port PORT` | RPC port for this node (default: HTTP port + 1000) |

### Starting HA

```bash
# Terminal 1 — Active Coordinator
./zepto_http_server --port 8123 --ha active --peer localhost:9101 --rpc-port 9100

# Terminal 2 — Standby Coordinator
./zepto_http_server --port 8124 --ha standby --peer localhost:9100 --rpc-port 9101

# Terminals 3~4 — Data Nodes (register with Active)
./zepto_data_node 9001 --coordinator localhost:8123 --api-key $ADMIN_KEY
./zepto_data_node 9002 --coordinator localhost:8123 --api-key $ADMIN_KEY
```

### Failover Behavior

1. Standby pings Active every 500ms
2. If no response for 2 seconds, Standby is automatically promoted to Active
3. The registered data node list is automatically re-registered with the new Active
4. Data node processes are unaffected (independent processes)
5. Clients connect to the new Active's HTTP port

### Client Switchover After Failover

When Active dies, Standby (8124) becomes the new Active:
```bash
# Before: http://localhost:8123
# After:  http://localhost:8124
```

In production, place a load balancer (ALB/NLB) in front and use health checks for automatic switchover.

---

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `Not in cluster mode` | Running an older version of the server | Update to the latest build (coordinator is always active) |
| Node shows as DEAD | Data node is down or port mismatch | Check the data node process and verify the port |
| Web UI shows only 1 node | Data node is not registered | Add via `--add-node` or `POST /admin/nodes` |
| `Connection refused` | Data node has not started yet | Start data nodes first, then start the coordinator |

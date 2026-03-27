# Multi-Node Cluster Guide

ZeptoDB 멀티 노드 클러스터 구성 및 운영 가이드.

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

- **Coordinator** (`zepto_http_server`): HTTP API + Web UI 제공, 쿼리를 scatter-gather로 분산
- **Data Node** (`zepto_data_node`): TCP RPC로 쿼리 수행, 데이터 저장

---

## Binaries

### zepto_http_server (Coordinator)

```
./zepto_http_server [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port`, `-p` | 8123 | HTTP 리스닝 포트 |
| `--ticks`, `-n` | 10000 | 시작 시 생성할 샘플 tick 수 |
| `--no-auth` | false | 인증 비활성화 |
| `--node-id` | 0 | 이 노드의 클러스터 ID |
| `--add-node id:host:port` | — | 원격 데이터 노드 등록 (여러 번 사용 가능) |
| `--log-level` | info | 로그 레벨 (debug/info/warn/error) |

Coordinator는 항상 활성화됩니다. Data node가 없으면 standalone으로 동작하고,
`--add-node`나 `POST /admin/nodes`로 노드가 추가되면 자동으로 cluster 모드로 전환됩니다.

### zepto_data_node (Data Node)

```
./zepto_data_node <port> [num_ticks] [options]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `port` | (필수) | TCP RPC 리스닝 포트 |
| `num_ticks` | 0 | 시작 시 생성할 샘플 tick 수 |
| `--node-id` | port 번호 | 클러스터 내 노드 ID |
| `--symbol` | 1 | 샘플 데이터의 symbol_id |
| `--coordinator host:port` | — | Coordinator에 자동 등록 |
| `--api-key` | — | 등록 시 사용할 admin API key |
| `--advertise-host` | localhost | Coordinator에 알릴 자신의 호스트 주소 |

---

## Quick Start

### 1. 빌드

```bash
cd build
ninja zepto_http_server zepto_data_node
```

### 2. 방법 A: Coordinator 먼저 시작 → Data Node가 자동 등록

```bash
# 터미널 1 — Coordinator
./zepto_http_server --port 8123
# admin API key가 출력됨 → 복사

# 터미널 2 — Data Node (자동 등록)
./zepto_data_node 9001 --coordinator localhost:8123 --api-key $ADMIN_KEY

# 터미널 3 — Data Node (자동 등록)
./zepto_data_node 9002 --coordinator localhost:8123 --api-key $ADMIN_KEY
```

### 3. 방법 B: 시작 시 모든 노드 지정

```bash
# 터미널 1~2 — Data Nodes 먼저 시작
./zepto_data_node 9001
./zepto_data_node 9002

# 터미널 3 — Coordinator (노드 연결)
./zepto_http_server --port 8123 \
  --add-node 9001:localhost:9001 \
  --add-node 9002:localhost:9002
```

### 4. Web UI 확인

브라우저에서 `http://localhost:3000/cluster` 접속 → 3개 노드 표시 (coordinator + data node 2개)

---

## Runtime Node Management

클러스터 시작 후에도 REST API로 노드를 동적으로 추가/제거할 수 있습니다.

### 노드 추가

```bash
# 새 데이터 노드 시작
./zepto_data_node 9003

# Coordinator에 등록
curl -X POST http://localhost:8123/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"id":9003,"host":"localhost","port":9003}'
```

### 노드 제거

```bash
curl -X DELETE http://localhost:8123/admin/nodes/9003 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

### 노드 목록 조회

```bash
curl http://localhost:8123/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY"
```

응답 예시:
```json
{
  "nodes": [
    {"id": 0, "host": "localhost", "port": 8123, "state": "ACTIVE", "ticks_ingested": 10000, "ticks_stored": 10000, "queries_executed": 5},
    {"id": 9001, "host": "localhost", "port": 9001, "state": "ACTIVE", "ticks_ingested": 0, "ticks_stored": 0, "queries_executed": 0},
    {"id": 9002, "host": "localhost", "port": 9002, "state": "ACTIVE", "ticks_ingested": 0, "ticks_stored": 0, "queries_executed": 0}
  ]
}
```

### 클러스터 상태 조회

```bash
curl http://localhost:8123/admin/cluster \
  -H "Authorization: Bearer $ADMIN_KEY"
```

응답 예시:
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

### 단일 노드 (Standalone)

```bash
./zepto_http_server --port 8123
```

클러스터 탭에 노드 1개 표시. `mode: standalone`. Data node를 추가하면 자동으로 `mode: cluster`로 전환.

### 2-Node Cluster

```bash
# Terminal 1
./zepto_data_node 9001

# Terminal 2
./zepto_http_server --port 8123 --add-node 9001:localhost:9001
```

### 4-Node Cluster

```bash
# Terminal 1~3: data nodes
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

# Machine A (10.0.1.2) — 자동 등록
./zepto_data_node 9001 \
  --coordinator 10.0.1.1:8123 \
  --api-key $ADMIN_KEY \
  --advertise-host 10.0.1.2

# Machine B (10.0.1.3) — 자동 등록
./zepto_data_node 9001 \
  --coordinator 10.0.1.1:8123 \
  --api-key $ADMIN_KEY \
  --advertise-host 10.0.1.3
```

---

## Web UI Cluster Tab

`/cluster` 페이지에서 확인할 수 있는 정보:

| Section | Description |
|---------|-------------|
| Summary Cards | 모드(standalone/cluster), 활성 노드 수, 총 tick 수, 총 쿼리 수 |
| Node Status Table | 노드별 ID, Host, Port, State, Ticks, Queries |
| Ticks Stored Bar Chart | 노드별 저장된 tick 수 막대 그래프 |
| Ingestion History | 노드별 시계열 ingestion 추이 |
| Queries History | 노드별 시계열 쿼리 실행 추이 |
| Latency History | 노드별 ingest latency 추이 |

노드 상태 색상:
- 🟢 ACTIVE — 정상
- 🟡 SUSPECT — 응답 지연
- 🔴 DEAD — 연결 불가
- 🔵 JOINING — 합류 중
- ⚪ LEAVING — 이탈 중

---

## High Availability (HA)

Active-Standby 구조로 coordinator 장애 시 자동 failover.

```
Coordinator (ACTIVE)  ←── ping (500ms) ──  Coordinator (STANDBY)
  port 8123, rpc 9100                        port 8124, rpc 9101
     │                                            │
     ├── Data Node 1 (9001)                       │
     └── Data Node 2 (9002)                       │
                                                  │
  ACTIVE 죽으면 → 2초 후 STANDBY가 ACTIVE로 승격
                   노드 목록 자동 재등록
```

### HA 옵션

| Option | Description |
|--------|-------------|
| `--ha active\|standby` | HA 역할 |
| `--peer host:port` | 상대 coordinator의 RPC 주소 |
| `--rpc-port PORT` | 이 노드의 RPC 포트 (기본: HTTP port + 1000) |

### HA 시작

```bash
# 터미널 1 — Active Coordinator
./zepto_http_server --port 8123 --ha active --peer localhost:9101 --rpc-port 9100

# 터미널 2 — Standby Coordinator
./zepto_http_server --port 8124 --ha standby --peer localhost:9100 --rpc-port 9101

# 터미널 3~4 — Data Nodes (Active에 등록)
./zepto_data_node 9001 --coordinator localhost:8123 --api-key $ADMIN_KEY
./zepto_data_node 9002 --coordinator localhost:8123 --api-key $ADMIN_KEY
```

### Failover 동작

1. Standby가 Active를 500ms 간격으로 ping
2. 2초간 응답 없으면 Standby → Active로 자동 승격
3. 등록된 data node 목록이 새 Active에 자동 재등록
4. Data node 프로세스는 영향 없음 (독립 프로세스)
5. 클라이언트는 새 Active의 HTTP 포트로 연결

### Failover 후 클라이언트 전환

Active가 죽으면 Standby(8124)가 새 Active가 됩니다:
```bash
# 기존: http://localhost:8123
# 전환: http://localhost:8124
```

프로덕션에서는 로드밸런서(ALB/NLB)를 앞에 두고 health check로 자동 전환합니다.

---

## Troubleshooting

| 증상 | 원인 | 해결 |
|------|------|------|
| `Not in cluster mode` | 이전 버전 서버 사용 중 | 최신 빌드로 업데이트 (coordinator 항상 활성화) |
| 노드가 DEAD 표시 | data node가 꺼져있거나 포트 불일치 | data node 프로세스 확인, 포트 확인 |
| Web UI에 노드 1개만 표시 | data node가 등록되지 않음 | `--add-node` 또는 `POST /admin/nodes`로 추가 |
| `Connection refused` | data node가 아직 시작 안됨 | data node를 먼저 시작한 후 coordinator 시작 |

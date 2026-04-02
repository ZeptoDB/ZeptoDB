# ZeptoDB Configuration Reference

*Last updated: 2026-03-25*

ZeptoDB uses a YAML configuration file for server startup. Pass it with `--config`:

```bash
./zepto_server --config /etc/zeptodb/config.yaml
```

All fields are optional — defaults are production-safe. Only override what you need.

---

## Table of Contents

- [Minimal Examples](#minimal-examples)
- [Full Reference](#full-reference)
  - [server](#server)
  - [auth](#auth)
  - [tls](#tls)
  - [pipeline](#pipeline)
  - [storage](#storage)
  - [cluster](#cluster)
  - [query](#query)
  - [feeds](#feeds)
  - [observability](#observability)
- [Environment Variable Overrides](#environment-variable-overrides)
- [Secrets Management](#secrets-management)

---

## Minimal Examples

### Development (single node, no auth)

```yaml
server:
  port: 8123

auth:
  enabled: false
```

### Production (single node, TLS + auth)

```yaml
server:
  port: 8123

tls:
  enabled: true
  cert_path: /etc/zeptodb/cert.pem
  key_path: /etc/zeptodb/key.pem
  https_port: 8443

auth:
  enabled: true
  api_keys_file: /etc/zeptodb/keys.txt
  jwt:
    enabled: true
    hs256_secret: ${JWT_SECRET}
    expected_issuer: https://auth.example.com
  rate_limit:
    requests_per_minute: 1000
    burst_capacity: 200

pipeline:
  storage_mode: tiered
  hdb_base_path: /data/zeptodb/hdb
```

### Multi-node cluster

```yaml
server:
  port: 8123

cluster:
  enabled: true
  node_id: 1
  host: 10.0.1.1
  port: 8123
  rpc_port: 8223
  peers:
    - { id: 2, host: 10.0.1.2, port: 8123 }
    - { id: 3, host: 10.0.1.3, port: 8123 }
  health:
    heartbeat_interval_ms: 1000
    suspect_timeout_ms: 3000
    dead_timeout_ms: 10000
```

---

## Full Reference

### server

HTTP server settings.

```yaml
server:
  port: 8123                    # HTTP listen port
  bind_address: 0.0.0.0        # Bind address (default: all interfaces)
  query_timeout_ms: 30000       # Auto-cancel queries after this duration (0 = no timeout)
  max_request_body_mb: 64       # Max POST body size in MB
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `port` | int | 8123 | HTTP listen port |
| `bind_address` | string | `0.0.0.0` | Network interface to bind |
| `query_timeout_ms` | int | 30000 | Query auto-cancel timeout (ms). 0 = disabled |
| `max_request_body_mb` | int | 64 | Maximum request body size |

---

### auth

Authentication and authorization.

```yaml
auth:
  enabled: true                       # false = bypass all auth (dev mode)
  api_keys_file: /etc/zeptodb/keys.txt

  jwt:
    enabled: false
    hs256_secret: ""                  # HS256 shared secret (mutually exclusive with rs256)
    rs256_public_key_pem: ""          # RS256 public key PEM file path
    expected_issuer: ""               # Validate JWT "iss" claim
    expected_audience: ""             # Validate JWT "aud" claim
    verify_expiry: true
    role_claim: zepto_role            # JWT claim name for role
    symbols_claim: zepto_symbols      # JWT claim name for symbol whitelist

  rate_limit:
    enabled: true
    requests_per_minute: 1000         # Per-identity limit
    burst_capacity: 200               # Max burst tokens
    per_ip_rpm: 10000                 # Per-IP limit (0 = disabled)
    ip_burst: 500

  audit:
    enabled: true
    log_file: /var/log/zeptodb/audit.log   # Empty = stderr via spdlog
    buffer_enabled: true                    # In-memory ring buffer for /admin/audit

  public_paths:                       # Paths that never require auth
    - /ping
    - /health
    - /ready
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | true | Master auth switch |
| `api_keys_file` | string | — | Path to API key store file |
| `jwt.enabled` | bool | false | Enable JWT/OIDC authentication |
| `jwt.hs256_secret` | string | — | HS256 shared secret |
| `jwt.rs256_public_key_pem` | string | — | RS256 public key PEM path |
| `jwt.expected_issuer` | string | — | Required JWT issuer |
| `jwt.expected_audience` | string | — | Required JWT audience |
| `jwt.role_claim` | string | `zepto_role` | JWT claim for role |
| `jwt.symbols_claim` | string | `zepto_symbols` | JWT claim for symbol list |
| `oidc.issuer` | string | — | OIDC issuer URL (auto-discovers JWKS, auth, token endpoints) |
| `oidc.client_id` | string | — | OAuth2 client ID |
| `oidc.client_secret` | string | — | OAuth2 client secret (use Vault/env in production) |
| `oidc.redirect_uri` | string | — | OAuth2 callback URL (e.g., `http://host:8123/auth/callback`) |
| `oidc.audience` | string | — | Expected audience claim (optional) |
| `session.enabled` | bool | false | Enable server-side session store |
| `session.ttl_s` | int | 3600 | Session lifetime (seconds) |
| `session.refresh_window_s` | int | 300 | Extend session if active within this window |
| `session.max_sessions` | int | 10000 | Max concurrent sessions |
| `session.cookie_name` | string | `zepto_sid` | Session cookie name |
| `session.cookie_secure` | bool | false | Secure flag on cookie (set true with TLS) |
| `session.cookie_httponly` | bool | true | HttpOnly flag on cookie |
| `sso.enabled` | bool | false | Enable multi-IdP SSO identity provider |
| `sso.cache_ttl_s` | int | 300 | Identity cache TTL |
| `sso.cache_capacity` | int | 10000 | Max cached identities |
| `rate_limit.requests_per_minute` | int | 1000 | Per-identity rate limit |
| `rate_limit.burst_capacity` | int | 200 | Token bucket burst size |
| `rate_limit.per_ip_rpm` | int | 10000 | Per-IP rate limit |
| `audit.log_file` | string | — | Audit log file path |

---

### tls

TLS/HTTPS configuration.

```yaml
tls:
  enabled: false
  cert_path: /etc/zeptodb/cert.pem
  key_path: /etc/zeptodb/key.pem
  ca_cert_path: ""                    # Optional: CA cert for mTLS
  https_port: 8443
  also_serve_http: true               # Keep HTTP on server.port when TLS enabled
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | false | Enable HTTPS |
| `cert_path` | string | — | Server certificate PEM |
| `key_path` | string | — | Private key PEM |
| `ca_cert_path` | string | — | CA cert for mutual TLS client verification |
| `https_port` | int | 8443 | HTTPS listen port |
| `also_serve_http` | bool | true | Also serve HTTP on `server.port` |

---

### pipeline

Core data pipeline settings.

```yaml
pipeline:
  storage_mode: in_memory             # in_memory | tiered | on_disk
  arena_size_mb: 32                   # Per-partition arena size
  drain_batch_size: 256               # Ticks per drain batch
  drain_threads: 1                    # Number of drain threads
  drain_sleep_us: 10                  # Drain thread sleep (μs)
  hdb_base_path: /data/zeptodb/hdb   # HDB root directory
  max_memory_mb: 0                    # 0 = unlimited, else evict oldest partitions

  recovery:
    enabled: false
    snapshot_path: /data/zeptodb/snapshots
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `storage_mode` | string | `in_memory` | `in_memory`, `tiered`, `on_disk` |
| `arena_size_mb` | int | 32 | Per-partition memory arena (MB) |
| `drain_batch_size` | int | 256 | Ticks drained per batch |
| `drain_threads` | int | 1 | Parallel drain threads |
| `hdb_base_path` | string | `/tmp/zepto_hdb` | Historical database directory |
| `max_memory_mb` | int | 0 | Memory limit (0 = unlimited) |
| `recovery.enabled` | bool | false | Load snapshot on startup |
| `recovery.snapshot_path` | string | — | Snapshot directory |

---

### storage

Flush, tiering, Parquet, and S3 settings.

```yaml
storage:
  flush:
    memory_threshold: 0.8             # Flush when arena usage exceeds this ratio
    check_interval_ms: 1000
    enable_compression: true          # LZ4 compression for binary HDB
    auto_seal_age_hours: 1            # Seal partitions older than N hours

  tiering:
    enabled: false
    warm_after_ns: 3600000000000      # 1 hour → warm (local SSD)
    cold_after_ns: 86400000000000     # 24 hours → cold (S3)
    drop_after_ns: 0                  # 0 = no auto-drop

  parquet:
    compression: snappy               # snappy | zstd | none
    row_group_size_mb: 128
    write_stats: true

  s3:
    enabled: false
    bucket: my-zepto-bucket
    prefix: hdb
    region: us-east-1
    endpoint_url: ""                  # Custom endpoint (MinIO, LocalStack)
    use_path_style: false             # true for MinIO
    delete_local_after_upload: false

  snapshot:
    enabled: false
    interval_ms: 60000                # Snapshot every 60s
    path: /data/zeptodb/snapshots
```

---

### cluster

Multi-node distributed cluster settings.

```yaml
cluster:
  enabled: false
  node_id: 1
  host: 10.0.1.1
  port: 8123
  rpc_port: 8223                      # TCP RPC port (0 = port + 100)
  enable_remote_ingest: true

  peers:
    - { id: 2, host: 10.0.1.2, port: 8123 }
    - { id: 3, host: 10.0.1.3, port: 8123 }

  health:
    heartbeat_interval_ms: 1000
    suspect_timeout_ms: 3000
    dead_timeout_ms: 10000
    heartbeat_port: 9100

  replication:
    factor: 2                         # Replication factor (1 = no replication)
    wal_batch_size: 1024

  coordinator_ha:
    enabled: false
    lease_name: zepto-coordinator
    lease_namespace: default
    lease_duration_s: 15
    renew_interval_s: 5
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | false | Enable cluster mode |
| `node_id` | int | — | Unique node identifier |
| `host` | string | — | This node's advertised hostname/IP |
| `rpc_port` | int | port+100 | TCP RPC port for inter-node communication |
| `health.heartbeat_interval_ms` | int | 1000 | Heartbeat send interval |
| `health.suspect_timeout_ms` | int | 3000 | Mark node SUSPECT after this silence |
| `health.dead_timeout_ms` | int | 10000 | Mark node DEAD after this silence |
| `replication.factor` | int | 2 | Number of copies per partition |

Nodes can also be added/removed at runtime via the Admin API:

```bash
# Add a node
curl -X POST https://zepto:8443/admin/nodes \
  -H "Authorization: Bearer $ADMIN_KEY" \
  -d '{"id":4,"host":"10.0.1.4","port":8123}'

# Remove a node
curl -X DELETE https://zepto:8443/admin/nodes/4 \
  -H "Authorization: Bearer $ADMIN_KEY"
```

---

### query

Query execution settings.

```yaml
query:
  parallel:
    enabled: true
    num_threads: 0                    # 0 = hardware_concurrency
    row_threshold: 100000             # Min rows for parallel execution

  resource_isolation:
    enabled: false
    realtime_cores: [0, 1]            # CPU cores for ingestion
    analytics_cores: [2, 3, 4, 5]    # CPU cores for queries
    drain_cores: [6]                  # CPU cores for drain threads
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `parallel.enabled` | bool | false | Enable parallel query execution |
| `parallel.num_threads` | int | 0 | Worker threads (0 = auto) |
| `parallel.row_threshold` | int | 100000 | Min rows to trigger parallel scan |
| `resource_isolation.realtime_cores` | int[] | — | CPU affinity for ingestion |
| `resource_isolation.analytics_cores` | int[] | — | CPU affinity for queries |

---

### feeds

External data feed handlers.

```yaml
feeds:
  kafka:
    enabled: false
    brokers: localhost:9092
    topic: market-data
    group_id: zepto-consumer
    auto_offset_reset: latest         # latest | earliest
    format: json                      # json | binary | itch
    price_scale: 10000.0
    poll_timeout_ms: 100
    batch_size: 1024
    commit_mode: after_ingest         # after_ingest | auto
    backpressure_retries: 3
    backpressure_sleep_us: 100
    symbol_map:
      AAPL: 1
      GOOGL: 2

  fix:
    enabled: false
    host: fix-gateway.example.com
    port: 9876
    username: zepto
    password: ${FIX_PASSWORD}
    use_tls: true
    reconnect_interval_ms: 5000
    heartbeat_interval_ms: 30000

  itch:
    enabled: false
    host: nasdaq-itch.example.com
    port: 1234
```

---

### observability

Logging, metrics, and monitoring.

```yaml
observability:
  log:
    level: info                       # trace | debug | info | warn | error
    directory: /var/log/zeptodb
    access_log: true                  # Structured JSON access log per request
    slow_query_ms: 100                # Log queries slower than this

  metrics:
    capture_interval_ms: 3000         # Internal metrics snapshot interval
    max_memory_bytes: 262144          # 256KB ring buffer for /admin/metrics/history
    response_limit: 600               # Max snapshots per API response

  prometheus:
    enabled: true                     # Expose /metrics endpoint
```

---

## Environment Variable Overrides

Any config value can be overridden with `${ENV_VAR}` syntax in the YAML file.
The server resolves these at startup.

```yaml
auth:
  jwt:
    hs256_secret: ${JWT_SECRET}

feeds:
  fix:
    password: ${FIX_PASSWORD}

storage:
  s3:
    bucket: ${S3_BUCKET}
```

Common environment variables:

| Variable | Description |
|----------|-------------|
| `ZEPTO_PORT` | Override `server.port` |
| `ZEPTO_CONFIG` | Config file path (alternative to `--config`) |
| `JWT_SECRET` | JWT HS256 shared secret |
| `FIX_PASSWORD` | FIX gateway password |
| `S3_BUCKET` | S3 bucket name |
| `AWS_REGION` | AWS region for S3 |

---

## Secrets Management

ZeptoDB resolves secrets using a priority chain:

```
1. HashiCorp Vault KV v2  (if VAULT_ADDR is set)
2. Kubernetes file secrets (if /var/run/secrets/zeptodb/ exists)
3. Environment variables   (fallback)
```

Configure Vault:

```yaml
secrets:
  vault:
    enabled: true
    address: https://vault.example.com:8200
    mount_path: secret/data/zeptodb
    token_path: /var/run/secrets/vault/token
```

All `${VAR}` references in the config file are resolved through this chain.
Secrets are never logged or exposed via API.

---

*See also: [HTTP API Reference](HTTP_REFERENCE.md) · [SQL Reference](SQL_REFERENCE.md) · [Production Deployment](../deployment/PRODUCTION_DEPLOYMENT.md)*

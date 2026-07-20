# License System Design

## Overview

ZeptoDB ships as a single binary. Edition-based feature gating is controlled at runtime by a signed license key. No key = Community Edition (free, unlimited).

## Edition Tiers

| Edition | Key Required | Max Nodes | Features |
|---------|:---:|:---:|---|
| Community | No | 1 | Core engine, SQL, HTTP API, Python DSL |
| Enterprise | Yes | Unlimited | + SSO, Audit Export, Advanced RBAC, Kafka, Migration, Cluster, Geo-Replication, Rolling Upgrade, IoT Connectors (MQTT / OPC-UA / ROS 2 / Kinesis) |

> **Backward compatibility:** Old license keys with `"edition": "pro"` are automatically treated as Enterprise. No action required from existing Pro licensees.

## License Key Format

RS256-signed JWT. Header: `{"alg":"RS256","typ":"JWT"}`.

Payload claims:

| Claim | Type | Description |
|-------|------|-------------|
| `edition` | string | `"enterprise"` or `"pro"` (backward compat); absent or other value = Community |
| `features` | uint32 | Bitmask of enabled Feature flags |
| `max_nodes` | int | Maximum cluster nodes allowed |
| `company` | string | Licensee company name |
| `tenant_id` | string | SaaS tenant isolation (future) |
| `exp` | int64 | Expiry Unix timestamp |
| `iat` | int64 | Issued-at Unix timestamp |
| `grace_days` | int | Post-expiry grace period (default 30) |

## Feature Bitmask

```
CLUSTER         = 1 << 0
SSO             = 1 << 1
AUDIT_EXPORT    = 1 << 2
ADVANCED_RBAC   = 1 << 3
KAFKA           = 1 << 4
MIGRATION       = 1 << 5
GEO_REPLICATION = 1 << 6
ROLLING_UPGRADE = 1 << 7
IOT_CONNECTORS  = 1 << 8
```

### IOT_CONNECTORS (bit 8)

Gates enterprise IoT / Physical AI streaming integrations. Currently gates:

- **MQTT consumer** (`MqttConsumer::start()`) — IoT broker ingestion
  (sensors, robotics, autonomous vehicles).
- **OPC-UA consumer** (`OpcUaConsumer::start()`) — industrial PLC / SCADA
  ingestion.
- **ROS 2 consumer** (`Ros2Consumer::start()`) — robotics /
  autonomous-driving middleware. Default/no-ROS builds validate and map data
  without a live ROS graph; `ZEPTO_ROS2_AVAILABLE` builds start live
  `std_msgs` scalar subscriptions.
- **AWS Kinesis consumer** (`KinesisConsumer::start()`) — AWS-native cloud
  stream ingestion. Default/no-SDK builds validate, decode, route, and expose
  metrics without live AWS credentials; `ZEPTO_KINESIS_AVAILABLE` builds poll
  one configured stream shard.

Planned future connectors under the same gate (no new feature bit needed):

- **Apache Pulsar** — alternative to Kafka.

Present in: Enterprise. Trial keys enable all features (bits 0..8 → `features=511`).

## Key Loading (Priority Order)

1. Environment variable `ZEPTODB_LICENSE_KEY` (JWT string)
2. File `/etc/zeptodb/license.key`
3. Direct string via `POST /admin/license`

If no key found or key is invalid: silently default to Community.

## Expiry Policy

- 7 days before expiry: `ZEPTO_WARN` log on each startup
- After expiry, within `grace_days` (default 30): continue running with warning
- After grace period: downgrade to Community edition

## Feature Gate API

```cpp
// Check feature availability
if (!zeptodb::auth::license().hasFeature(Feature::CLUSTER)) {
    return http_402("enterprise_required", "Cluster requires Enterprise license");
}
```

## HTTP 402 Response

Gated endpoints return:
```json
{
  "error": "enterprise_required",
  "message": "Multi-node cluster requires Enterprise license",
  "upgrade_url": "https://zeptodb.com/pricing"
}
```

## Security

- License public key is embedded at compile time
- Non-trial keys require an exact three-part JWT with `alg=RS256`; the signature
  is verified over the original header and payload bytes with the embedded key
- License claims use their own schema and are not interpreted as login identity
  claims
- No phone-home; validation is fully offline

## Trial Keys

Trial keys allow 30-day evaluation of Enterprise features without requiring a signed license key.

### Format

Unsigned JWT with `"alg":"none"` header and `"trial":true` claim:

| Claim | Value |
|-------|-------|
| `alg` | `"none"` (no signature) |
| `edition` | `"enterprise"` |
| `features` | `0x1FF` (all features: bits 0..8) |
| `max_nodes` | `1` (single-node only) |
| `trial` | `true` |
| `exp` | now + 30 days |

### Validation Rules

- The validator accepts unsigned JWTs only when both `alg=none` AND `trial=true`
- Expiry is still enforced — expired trial keys are rejected (with grace period)
- Trial keys are limited to single-node (`max_nodes=1`)
- `isTrial()` returns true; `statusLine()` includes "(Trial)" suffix

### Generation

```cpp
std::string key = LicenseValidator::generate_trial_key("MyCompany");
license().load(key);
```

Or via HTTP:
```bash
curl -X POST http://localhost:8123/admin/license/trial
```

# ZeptoDB Backlog

> Completed features list: [`COMPLETED.md`](COMPLETED.md)

> Client API Compatibility Matrix: [`docs/design/client_compatibility.md`](docs/design/client_compatibility.md)
 ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` | ❌ | ❌ | ✅ `/admin/audit` |

---

## High Priority — Technical

- [ ] **Bare-metal tuning detailed guide** — CPU pinning, NUMA, io_uring

---

## High Priority — Business / Operations

- [ ] **Web UI / Admin Console** — Query editor, cluster status, demo/PoC impact
- [ ] **Website & documentation site** — zeptodb.io, docs.zeptodb.io
- [ ] **Limited DSL AOT compilation** — Nuitka → single binary, Cython → C extensions

---

## Medium Priority

- [ ] **Vault-backed API Key Store** — API 키를 keys.txt 파일 대신 HashiCorp Vault KV v2에 저장/조회. SecretsProvider 체인과 통합, 키 rotation 지원
- [ ] **JDBC/ODBC drivers** — Tableau, Excel, Power BI, Java/.NET Ecosystem
- [ ] **CDC connector** — PostgreSQL/MySQL → ZeptoDB Real-time sync (Debezium/Kafka Connect)
- [ ] **Geo-replication** — multi-region Replication, Global trading desk
- [ ] **User-Defined Functions** — Python/WASM UDF, Custom alpha signals
- [ ] **Cloud Marketplace** — AWS/GCP Marketplace registration, Enterprise procurement simplification
- [ ] **Multi-node benchmark execution** — EKS guide ready, Helm bench-values + eksctl config; ~$12/run
- [ ] **DuckDB embedding** — delegate complex JOINs via Arrow zero-copy
- [ ] **JIT SIMD emit** — generate AVX2/512 vector IR from LLVM JIT
- [ ] **Arrow Flight server** — stream query results as Arrow batches; direct Pandas/Polars client
- [ ] **ClickHouse wire protocol** — binary protocol compatibility

---

## Storage & Format

- [ ] **Arrow Flight server** — see Medium Priority above

---

## Streaming Data Integration

- [ ] **Kafka Connect Sink** — register as Kafka Connect sink plugin
- [ ] **Apache Pulsar consumer**
- [ ] **AWS Kinesis consumer**

---

## Physical AI / Industry

- [ ] **ROS2 plugin** — ROS2 topics → ZeptoDB ingestion
- [ ] **OPC-UA connector** — Siemens S7, industrial PLC
- [ ] **MQTT ingestion** — IoT device connection

---

## Distributed Query & Cluster

- [ ] **Live rebalancing** — zero-downtime partition movement
- [ ] **Tier C cold query offload** — recent → APEX in-memory, old → DuckDB on S3
- [ ] **PTP clock sync detection** — enforce for ASOF JOIN strict mode

---

## Multi-Usecase Extensions

- [ ] **Pluggable partition strategy** — symbol_affinity / hash_mod / site_id
- [ ] **Edge mode** (`--mode edge`) — single-node with async cloud sync
- [ ] **HyperLogLog** — distributed approximate COUNT DISTINCT

---

## Low Priority

- [ ] HDB Compaction — Parquet flush already works; merge via Glue/Spark/parquet-tools externally
- [ ] Snowflake/Delta Lake hybrid support
- [ ] AWS Fleet API integration (Warm Pool + Placement Group)
- [ ] DynamoDB metadata (partition map)
- [ ] Graph index (CSR) — fund flow tracking
- [ ] InfluxDB migration (InfluxQL → SQL)

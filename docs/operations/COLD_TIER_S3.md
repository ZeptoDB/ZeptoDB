# Cold tier — S3 Parquet sink

> Stream sealed/aged ZeptoDB partitions to S3 as Hive-partitioned Parquet,
> so external tools (Athena, DuckDB, Polars, Spark) can query the cold tier
> directly while ZeptoDB owns the hot tier in memory.
>
> Devlog: [`118_s3_parquet_sink_connector.md`](../devlog/118_s3_parquet_sink_connector.md).
> Lower-level activation guide: [`../deployment/PARQUET_S3_ACTIVATION.md`](../deployment/PARQUET_S3_ACTIVATION.md).

---

## 1. What it is

ZeptoDB's `FlushManager` already writes Parquet locally and uploads it to S3
since devlog 012. Devlog 118 adds the operator surface: a Hive-partitioned
S3 path layout (`year=…/month=…/day=…/symbol=…/`), a single Helm
`coldTier:` block, matching `--cold-tier-*` CLI flags, and `ZEPTO_COLD_TIER_*`
env vars. With one flag, sealed partitions older than `ageHours` flush to
local Parquet and upload to S3, optionally deleting the local copy on
success. The default Hive layout is auto-discovered by every major Parquet
reader; FLAT is kept for byte-identical backward compat with pre-118
deployments.

---

## 2. Enable via Helm (one command)

```bash
helm install zepto deploy/helm/zeptodb \
  --set coldTier.enabled=true \
  --set coldTier.s3.bucket=my-zepto-cold \
  --set coldTier.s3.region=us-east-1 \
  --set coldTier.s3.prefix=hdb \
  --set coldTier.layout=hive \
  --set coldTier.ageHours=24 \
  --set coldTier.deleteLocalAfterS3=true
```

The chart wires `ZEPTO_COLD_TIER_*` env vars onto the `zepto_http_server`
container (both `Deployment` and `StatefulSet` paths), and surfaces the same
keys in the configmap for operator visibility.

For MinIO / non-AWS endpoints:

```bash
helm install zepto deploy/helm/zeptodb \
  --set coldTier.enabled=true \
  --set coldTier.s3.bucket=zepto \
  --set coldTier.s3.endpointUrl=https://minio.example.com \
  --set coldTier.s3.usePathStyle=true \
  --set coldTier.s3.region=us-east-1
```

---

## 3. Enable via CLI flags

For non-K8s deployments, pass the same knobs directly to `zepto_http_server`:

```bash
zepto_http_server \
  --port 8123 \
  --hdb-dir /opt/zepto/hdb \
  --cold-tier-enabled \
  --cold-tier-format parquet \
  --cold-tier-layout hive \
  --cold-tier-age-hours 24 \
  --cold-tier-delete-local-after-s3 \
  --cold-tier-s3-bucket my-zepto-cold \
  --cold-tier-s3-region us-east-1 \
  --cold-tier-s3-prefix hdb
```

CLI flags win over `ZEPTO_COLD_TIER_*` env vars. When `--cold-tier-enabled`
is set without an explicit `--storage-mode tiered`, the server auto-promotes
the storage mode to `TIERED` so the `FlushManager` actually runs.

The startup log emits a single INFO line listing the resolved cold-tier
config (no secrets):

```
Cold tier S3 Parquet sink ENABLED: format=parquet layout=hive age_hours=24
    delete_local=true bucket=my-zepto-cold region=us-east-1 prefix=hdb
    endpoint=<aws> path_style=false
```

---

## 4. S3 IAM minimum policy

The pod / process needs PUT access to the configured prefix. The standard
"upload-only" policy is below. Add `s3:GetObject` if you intend to read the
data back through ZeptoDB itself (the default reader path is to use external
tools — Athena/DuckDB/Polars/Spark — pointed at the bucket).

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "s3:PutObject",
        "s3:AbortMultipartUpload"
      ],
      "Resource": "arn:aws:s3:::my-zepto-cold/hdb/*"
    },
    {
      "Effect": "Allow",
      "Action": [
        "s3:ListBucket"
      ],
      "Resource": "arn:aws:s3:::my-zepto-cold"
    },
    {
      "Comment": "Optional — only needed for ZeptoDB-side reads",
      "Effect": "Allow",
      "Action": [
        "s3:GetObject"
      ],
      "Resource": "arn:aws:s3:::my-zepto-cold/hdb/*"
    }
  ]
}
```

On EKS, attach the policy to an IRSA role and bind the service account to
the StatefulSet/Deployment via `serviceAccountName`. On EC2, use the
instance-profile role. Static AWS access keys via env vars are supported but
discouraged.

---

## 5. Reading the data

The default `coldTier.layout=hive` emits paths in the form:

```
s3://{bucket}/{prefix}/year=YYYY/month=MM/day=DD/symbol={ID}/{ID}-{hour_epoch}[-{hash}].parquet
```

Every column is encoded as Apache Parquet (SNAPPY by default, configurable).
The Hive directory partitioning (`year=…/month=…/…`) is what every modern
columnar engine auto-discovers.

### Athena (AWS)

```sql
CREATE EXTERNAL TABLE zepto_cold (
    timestamp BIGINT,
    price     BIGINT,
    volume    BIGINT,
    msg_type  INT
)
PARTITIONED BY (
    year   STRING,
    month  STRING,
    day    STRING,
    symbol STRING
)
STORED AS PARQUET
LOCATION 's3://my-zepto-cold/hdb/';

-- Discover partitions on demand:
MSCK REPAIR TABLE zepto_cold;

-- Or use partition projection (faster, no MSCK):
ALTER TABLE zepto_cold SET TBLPROPERTIES (
  'projection.enabled'        = 'true',
  'projection.year.type'      = 'integer',
  'projection.year.range'     = '2024,2030',
  'projection.month.type'     = 'integer',
  'projection.month.range'    = '1,12',
  'projection.month.digits'   = '2',
  'projection.day.type'       = 'integer',
  'projection.day.range'      = '1,31',
  'projection.day.digits'     = '2',
  'projection.symbol.type'    = 'integer',
  'projection.symbol.range'   = '1,1000000',
  'storage.location.template' =
    's3://my-zepto-cold/hdb/year=${year}/month=${month}/day=${day}/symbol=${symbol}/'
);

SELECT symbol, COUNT(*) AS rows
FROM zepto_cold
WHERE year='2026' AND month='05'
GROUP BY symbol;
```

### DuckDB

```sql
INSTALL httpfs;
LOAD httpfs;
SET s3_region='us-east-1';

SELECT count(*)
FROM read_parquet('s3://my-zepto-cold/hdb/**/*.parquet',
                  hive_partitioning = 1)
WHERE year = '2026' AND month = '05';
```

### Polars (Python)

```python
import polars as pl

lf = pl.scan_parquet(
    "s3://my-zepto-cold/hdb/**/*.parquet",
    hive_partitioning=True,
)

df = (
    lf.filter((pl.col("year") == "2026") & (pl.col("month") == "05"))
      .group_by("symbol")
      .agg(pl.len().alias("rows"))
      .collect()
)
```

### Spark / PySpark

```python
df = (
    spark.read.parquet("s3://my-zepto-cold/hdb/")
         .filter("year = '2026' AND month = '05'")
)
df.groupBy("symbol").count().show()
```

(Spark Parquet auto-discovers Hive partitions by default.)

---

## 6. Cost note

S3 cost has three drivers worth tuning:

1. **PUT requests.** Each sealed partition is one `PutObject` per Parquet
   file. With many symbols and `coldTier.ageHours=24` you get one PUT per
   `(symbol, hour)` per day. At $0.005 per 1k PUTs (Standard, us-east-1)
   this is negligible for typical workloads, but the `ageHours` knob and
   the `format` knob (`parquet` vs `both`) both directly drive the PUT
   count — increasing `ageHours` halves the PUT rate.
2. **Storage class.** Cold-tier data is rarely re-read; the S3
   Standard-IA or One Zone-IA storage classes are typically 40–60% cheaper
   per GB than Standard. Set this on the bucket via a lifecycle rule:

   ```yaml
   # lifecycle.yaml
   Rules:
     - ID: zepto-ia-after-30d
       Status: Enabled
       Filter:
         Prefix: hdb/
       Transitions:
         - Days: 30
           StorageClass: STANDARD_IA
         - Days: 180
           StorageClass: GLACIER_IR
   ```

3. **Egress + Athena scans.** Athena charges per TB scanned. With Hive
   partition pruning (`year=…/month=…`) and Parquet columnar reads, well-
   filtered queries scan a small fraction of the bucket. Use `MSCK REPAIR`
   sparingly or prefer partition projection (above).

If you also keep the local Parquet files (`coldTier.deleteLocalAfterS3=false`),
budget local disk for the retention window. The default
`deleteLocalAfterS3=true` is the cheap path: S3 is the durable copy, ZeptoDB
keeps the hot rows in memory.

---

## See also

- [Devlog 118 — implementation record](../devlog/118_s3_parquet_sink_connector.md)
- [Devlog 012 — Parquet + S3 design](../devlog/012_parquet_s3.md)
- [PARQUET_S3_ACTIVATION.md — lower-level / library activation](../deployment/PARQUET_S3_ACTIVATION.md)
- [layer1_storage_memory.md — cold-tier subsection](../design/layer1_storage_memory.md)

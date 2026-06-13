# 175: P5 AWS Kinesis Consumer

Date: 2026-06-12
Status: Complete

## Context

P5 Data Pipelines needed an AWS-native streaming connector after the Kafka,
MQTT, S3, and Telegraf surfaces. The smallest useful cut was to match the
existing feed pattern: keep decode and routing testable without live cloud
credentials, and enable real polling only when the optional SDK is present.

## Changes

- Added `KinesisConfig`, `KinesisStats`, and `KinesisConsumer`.
- Reused Kafka's `JSON`, `BINARY`, and `JSON_HUMAN` decoders as the single
  source of truth for tick payload formats.
- Added table-aware ingest, backpressure retries, single-node routing, cluster
  routing, and Prometheus metrics for Kinesis records.
- Added optional `-DZEPTO_USE_KINESIS=ON` CMake support. Default builds compile
  decode/routing logic and return `false` from `start()` when AWS SDK Kinesis is
  absent.
- Updated the layer 2 design doc, license-system design, C++ API reference,
  backlog, and completed-feature log.

## Verification

- `cmake --build build -j$(nproc) --target zepto_tests` — PASS.
- `./build/tests/zepto_tests --gtest_filter='KinesisConsumerTest.*'` — PASS,
  14 tests.
- Full local x86_64 `./build/tests/zepto_tests` — PASS, 1477 passed,
  1 live S3 opt-in skipped, 3 disabled.
- aarch64 / Graviton pre-push `zepto_tests` — PASS, 1464 tests,
  0 failures, 0 errors, 3 disabled.

## Follow-ups

- P5 next priority is the Apache Pulsar consumer.
- Live AWS integration smoke should be added once a test Kinesis stream and
  credentials are available in CI or EKS.

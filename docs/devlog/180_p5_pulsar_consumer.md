# 180: P5 Apache Pulsar Consumer

Date: 2026-06-13
Status: Complete

## Context

The P5 data-pipeline backlog still had Apache Pulsar as the next connector.
Kafka, MQTT, and Kinesis already share a stable pattern for broker payload
decode, table-aware routing, backpressure, and default builds that remain
testable without optional SDKs. Pulsar needed to join that connector family so
Physical AI and IoT deployments that standardize on Pulsar can ingest directly
into ZeptoDB.

## Changes

- Added `include/zeptodb/feeds/pulsar_consumer.h` and
  `src/feeds/pulsar_consumer.cpp`.
- Added `PulsarConfig`, `PulsarConsumer`, `PulsarStats`,
  `PulsarSubscriptionType`, and `PulsarInitialPosition`.
- Reused Kafka's JSON, BINARY, and JSON_HUMAN decoders so the wire contract
  stays identical across Kafka, MQTT, Kinesis, and Pulsar.
- Added table-aware single-node and cluster routing with backpressure retries.
- Added Prometheus metric formatting for consumed messages, bytes, decode
  errors, routing, ingest failures, receive timeouts/errors, and ack errors.
- Added optional CMake detection through `-DZEPTO_USE_PULSAR=ON`; default builds
  compile the decode/routing path and return false from `start()` when the
  Pulsar C++ client is absent.
- Updated C++/config/design docs and closed the P5 backlog row.

## Verification

- `cmake --build build -j$(nproc) --target zepto_tests` - PASS.
- `./build/tests/zepto_tests --gtest_filter='PulsarConsumerTest.*'` - PASS,
  21 tests.
- `./build/tests/zepto_tests` - PASS, 1499 tests run, 1498 passed,
  1 live S3 opt-in skipped, 3 disabled.

## Follow-ups

- P5 now has two open rows: Debezium CDC connector and Kafka Connect Sink.

# Experiment 018: C++ Connector Two-Node Edge/Fleet Replay

Status: PASS

This run connects the experimental `EdgeFleetFeedConnector` to two live ZeptoDB HTTP nodes. The edge node is seeded from the Physical AI SQL fixture, the C++ connector reads the edge outbox through native SQL, and the fleet node receives inbox, materialized operational rows, ACKs, and feed telemetry through SQL inserts.

## Inputs

- Edge URL: `http://127.0.0.1:19441`
- Fleet URL: `http://127.0.0.1:19442`
- Outage URL: `http://127.0.0.1:1`
- Edge SQL statements applied: 148
- Fleet seed SQL statements applied: 51
- Connector batch limit: 12
- Connector max inflight: 12
- Edge outbox events: 52
- Checkpoint: `/tmp/zeptodb_edge_fleet_cpp_connector_018.checkpoint`

## Pass Telemetry

| phase | pass | batch | attempted | acked | failed | dropped | duplicates | late | acked before | acked after | restart reload |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| outage_probe | 1 | 12 | 12 | 0 | 12 | 0 | 0 | 0 | 0 | 0 | 0 |
| bounded_recovery_with_drop_duplicate | 2 | 12 | 12 | 11 | 1 | 1 | 1 | 0 | 0 | 11 | 0 |
| restart_retry_late_delivery | 3 | 12 | 12 | 12 | 0 | 0 | 11 | 1 | 11 | 23 | 1 |
| bounded_final_drain | 4 | 12 | 12 | 12 | 0 | 0 | 23 | 0 | 23 | 35 | 0 |
| bounded_final_drain | 5 | 12 | 12 | 12 | 0 | 0 | 35 | 0 | 35 | 47 | 0 |
| bounded_final_drain | 6 | 5 | 5 | 5 | 0 | 0 | 47 | 0 | 47 | 52 | 0 |

## Native SQL Validation

| check | value |
| --- | ---: |
| edge outbox rows | 52 |
| fleet inbox rows | 52 |
| fleet ACK rows | 52 |
| fleet decision rows | 5 |
| fleet retrieval rows | 15 |
| fleet suppression rows | 32 |
| fleet telemetry rows | 6 |
| outage telemetry rows | 1 |
| duplicate telemetry rows | 5 |
| late telemetry rows | 1 |
| restart telemetry rows | 1 |
| recovery JOIN rows | 5 |
| suppression audit JOIN rows | 32 |

## Interpretation

The C++ connector preserves edge-local replay state under outage, bounded drop, duplicate, late, and restart phases. Fleet-global audit rows converge after checkpoint reload, and the native JOIN checks prove that recovery recommendations and suppression audit rows remain queryable after materialization.

# 142 — ROS 2 / Physical AI Roadmap

**Date:** 2026-05-30
**Scope:** Documentation roadmap
**Status:** Complete

## Summary

ZeptoDB now has a formal ROS 2 / Physical AI roadmap anchored in
`docs/design/ros2_physical_ai_roadmap.md`.

The roadmap promotes Physical AI from a marketing/use-case direction into an
implementation plan:

- ROS 2 connector skeleton with optional `ZEPTO_USE_ROS2`
- live `rclcpp` scalar subscriber MVP
- rosbag2 import and deterministic replay
- standard message profiles for IMU, JointState, Odometry, TF, and LaserScan
- schema-aware typed ingest for wider robot messages
- Isaac Sim / digital twin ingestion with `/clock` semantics
- reference examples for robot RL replay, LiDAR ASOF JOIN, and fleet anomaly
- robot/factory edge deployment guidance

## Design Decisions

1. **ZeptoDB does not replace ROS 2.** It sits beside DDS and the ROS 2 graph as
   a time-series data layer for replay, feature extraction, observability, and
   Python analytics.
2. **Read-only bridge first.** The MVP subscribes to telemetry topics and does
   not publish actuator commands. Any future command publishing must be a
   separate audited capability.
3. **Time provenance is mandatory.** Source time, receive time, simulation
   time, replay time, session ID, and clock domain are first-class design
   concerns.
4. **Start narrow, then widen.** The first implementation path can use scalar
   field extraction into the existing ingest contract. Typed wide tables should
   follow once the generic ingest surface is ready.
5. **Large payloads need a different path.** Camera frames and full point
   clouds should start as metadata plus payload references, not hot-path blobs.

## Files Changed

| File | Change |
|---|---|
| `docs/design/ros2_physical_ai_roadmap.md` | New roadmap and technical design anchor |
| `docs/BACKLOG.md` | Split the single ROS 2 plugin row into concrete P9 implementation slices |
| `mkdocs.yml` | Added the roadmap to the published docs navigation |
| `docs/design/high_level_architecture.md` | Added ROS 2 / Physical AI to the roadmap table |
| `docs/design/physical_ai_market.md` | Linked Phase 3 strategy to the technical roadmap |
| `docs/index.md` | Added a docs-home link to the roadmap |
| `README.md` | Added Physical AI / Robotics use-case row |

## Follow-up Implementation Order

1. `1a` — ROS 2 connector skeleton
2. `1b` — live scalar subscriber MVP
3. `1c` — rosbag2 import/replay
4. `1d` — standard message profiles
5. `1f` and `1g` — Isaac Sim recipe and reference examples

## Verification

No build or test command was run. This was a documentation-only roadmap
change.

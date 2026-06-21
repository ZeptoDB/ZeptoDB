# 179: Physical AI Agent Memory Demo

Date: 2026-06-13
Status: Complete

## Context

Agent Memory examples already covered provider cache, LangGraph-style turns,
AgentOps telemetry, and multi-vertical time-series demos. Physical AI needed a
more concrete example that loads realistic robotics, logistics, and cold-chain
rows, then shows how an attached agent retrieves prior operational memory before
making a decision.

## Changes

- Added `examples/agent_memory/physical_ai_agent_demo.py`.
- The demo includes warehouse AGV/pallet timelines, ROS odometry plus LaserScan
  replay, and cold-chain shipment exception telemetry.
- Each scenario creates ordinary time-series tables, inserts realistic rows,
  seeds scoped Agent Memory records, retrieves context, builds an agent prompt,
  and records AgentOps context trace/replay rows.
- Documented the demo from both Agent Memory and ROS 2 example READMEs.
- Added pytest coverage for all Physical AI scenarios, multi-table inserts,
  memory metadata, context trace/replay SQL, empty table handling, and epoch 0
  timestamp handling.

## Verification

- `python3 -m py_compile examples/agent_memory/physical_ai_agent_demo.py`.
- `python3 -m pytest tests/python/test_agent_memory_examples.py
  tests/python/test_agent_memory_client.py -q` — PASS, 20 tests.

## Follow-ups

- Run the demo against a live ZeptoDB server with `--agent-memory-dir` and a
  larger rosbag2-derived dataset.

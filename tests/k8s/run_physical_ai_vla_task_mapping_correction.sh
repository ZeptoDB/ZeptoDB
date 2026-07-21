#!/bin/bash
# Run shadow-only Experiment 034 with explicit task-identity attribution.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=034
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_task_mapping_correction.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_veto_separability_034
export ZEPTO_VLA_DETAIL_RESULT_STEM=physical_ai_vla_veto_separability_candidates_034
export ZEPTO_VLA_REQUIRE_DIAGNOSTIC_ONLY=1
export ZEPTO_VLA_REQUIRE_TASK_MAPPING_CORRECTION=1

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"

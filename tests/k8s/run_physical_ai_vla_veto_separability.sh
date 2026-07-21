#!/bin/bash
# Run forced-shadow Experiment 033 with the shared staged EKS runner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=033
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_veto_separability.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_veto_separability_033
export ZEPTO_VLA_DETAIL_RESULT_STEM=physical_ai_vla_veto_separability_candidates_033
export ZEPTO_VLA_REQUIRE_DIAGNOSTIC_ONLY=1

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"

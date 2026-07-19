#!/bin/bash
# Run research-only Experiment 031 with the shared staged EKS runner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=031
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_risk_router.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_risk_router_031

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"

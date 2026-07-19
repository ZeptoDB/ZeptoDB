#!/bin/bash
# Run research-only Experiment 029 with the shared staged EKS runner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ZEPTO_VLA_EXPERIMENT=029
export ZEPTO_VLA_PYTHON_SCRIPT=physical_ai_vla_trajectory_fork.py
export ZEPTO_VLA_RESULT_STEM=physical_ai_vla_trajectory_fork_029

exec "$SCRIPT_DIR/run_physical_ai_vla_closed_loop.sh" "$@"

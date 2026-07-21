#!/usr/bin/env python3
"""Run shadow-only Experiment 032 calibration failure attribution."""

from __future__ import annotations

import physical_ai_vla_risk_router as risk_router


EXPERIMENT = 32


def main() -> int:
    return risk_router.main(
        default_experiment_id=EXPERIMENT,
        force_experiment_id=EXPERIMENT,
        force_diagnostic_only=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run forced-shadow Experiment 033 negative-veto separability diagnostics."""

from __future__ import annotations

import physical_ai_vla_risk_router as risk_router


EXPERIMENT = 33


def main() -> int:
    return risk_router.main(
        default_experiment_id=EXPERIMENT,
        force_experiment_id=EXPERIMENT,
        force_diagnostic_only=True,
        force_veto_separability=True,
    )


if __name__ == "__main__":
    raise SystemExit(main())

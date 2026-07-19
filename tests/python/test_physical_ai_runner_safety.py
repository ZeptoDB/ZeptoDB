"""Tests for fail-closed Physical AI EKS runner helpers."""

from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
HELPER = PROJECT_ROOT / "tests" / "k8s" / "require_zepto_bench_context.sh"
EXPECTED_ACCOUNT = "060795905711"
EXPECTED_ENDPOINT = "https://expected.eks.amazonaws.com"


def _write_command(directory: Path, name: str, body: str) -> None:
    path = directory / name
    path.write_text("#!/bin/sh\nset -eu\n" + body)
    path.chmod(0o755)


def _run_guard(
    tmp_path: Path,
    *,
    account: str = EXPECTED_ACCOUNT,
    aws_endpoint: str = EXPECTED_ENDPOINT,
    kube_endpoint: str = EXPECTED_ENDPOINT,
    include_aws: bool = True,
    include_kubectl: bool = True,
) -> subprocess.CompletedProcess[str]:
    if include_aws:
        _write_command(
            tmp_path,
            "aws",
            """
case "$1 $2" in
  "sts get-caller-identity") printf '%s\\n' "$MOCK_AWS_ACCOUNT" ;;
  "eks describe-cluster") printf '%s\\n' "$MOCK_AWS_ENDPOINT" ;;
  *) exit 2 ;;
esac
""",
        )
    if include_kubectl:
        _write_command(
            tmp_path,
            "kubectl",
            "printf '%s' \"$MOCK_KUBE_ENDPOINT\"\n",
        )

    command = f"source {shlex.quote(str(HELPER))}; zepto_require_bench_context"
    return subprocess.run(
        ["/bin/bash", "-c", command],
        check=False,
        capture_output=True,
        text=True,
        env={
            "PATH": str(tmp_path),
            "MOCK_AWS_ACCOUNT": account,
            "MOCK_AWS_ENDPOINT": aws_endpoint,
            "MOCK_KUBE_ENDPOINT": kube_endpoint,
        },
    )


def test_context_guard_accepts_exact_account_and_endpoint(tmp_path: Path) -> None:
    result = _run_guard(tmp_path)

    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("missing", ["aws", "kubectl"])
def test_context_guard_rejects_missing_command(tmp_path: Path, missing: str) -> None:
    result = _run_guard(
        tmp_path,
        include_aws=missing != "aws",
        include_kubectl=missing != "kubectl",
    )

    assert result.returncode != 0
    assert f"Required command is unavailable: {missing}" in result.stderr


def test_context_guard_rejects_wrong_account(tmp_path: Path) -> None:
    result = _run_guard(tmp_path, account="000000000000")

    assert result.returncode != 0
    assert f"outside AWS account {EXPECTED_ACCOUNT}" in result.stderr


@pytest.mark.parametrize("endpoint", ["", "https://other.eks.amazonaws.com"])
def test_context_guard_rejects_empty_or_wrong_endpoint(
    tmp_path: Path,
    endpoint: str,
) -> None:
    result = _run_guard(tmp_path, kube_endpoint=endpoint)

    assert result.returncode != 0
    assert "outside EKS cluster zepto-bench" in result.stderr


def test_no_clobber_publish_preserves_existing_target(tmp_path: Path) -> None:
    source = tmp_path / "source.md"
    target = tmp_path / "target.md"
    source.write_text("new evidence\n")
    target.write_text("frozen evidence\n")
    command = (
        f"source {shlex.quote(str(HELPER))}; "
        f"zepto_publish_no_clobber {shlex.quote(str(source))} "
        f"{shlex.quote(str(target))}"
    )

    result = subprocess.run(
        ["/bin/bash", "-c", command],
        check=False,
        capture_output=True,
        text=True,
        env={**os.environ},
    )

    assert result.returncode != 0
    assert target.read_text() == "frozen evidence\n"
    assert "Refusing to overwrite immutable experiment result" in result.stderr


def test_no_clobber_publish_links_new_target(tmp_path: Path) -> None:
    source = tmp_path / "source.md"
    target = tmp_path / "nested" / "target.md"
    source.write_text("new evidence\n")
    command = (
        f"source {shlex.quote(str(HELPER))}; "
        f"zepto_publish_no_clobber {shlex.quote(str(source))} "
        f"{shlex.quote(str(target))}"
    )

    result = subprocess.run(
        ["/bin/bash", "-c", command],
        check=False,
        capture_output=True,
        text=True,
        env={**os.environ},
    )

    assert result.returncode == 0, result.stderr
    assert target.read_text() == "new evidence\n"

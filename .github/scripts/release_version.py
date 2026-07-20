#!/usr/bin/env python3
"""Calculate and apply ZeptoDB release versions."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def parse_version(value: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(value)
    if not match:
        raise ValueError(f"expected MAJOR.MINOR.PATCH, got {value!r}")
    return tuple(int(part) for part in match.groups())


def format_version(version: tuple[int, int, int]) -> str:
    return ".".join(str(part) for part in version)


def current_git_tag_versions() -> list[tuple[int, int, int]]:
    result = subprocess.run(
        ["git", "tag", "-l", "v*"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    versions: list[tuple[int, int, int]] = []
    for tag in result.stdout.splitlines():
        match = TAG_RE.fullmatch(tag.strip())
        if match:
            versions.append(tuple(int(part) for part in match.groups()))
    return versions


def read_cmake_version(root: Path) -> tuple[int, int, int]:
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\(\s*zeptodb\s*\n\s*VERSION\s+(\d+\.\d+\.\d+)",
        text,
        flags=re.MULTILINE,
    )
    if not match:
        raise ValueError("could not find project VERSION in CMakeLists.txt")
    return parse_version(match.group(1))


def read_python_version(root: Path) -> tuple[int, int, int]:
    text = (root / "zepto_py" / "__init__.py").read_text(encoding="utf-8")
    match = re.search(r'^__version__ = "([^"]+)"$', text, flags=re.MULTILINE)
    if not match:
        raise ValueError("could not find __version__ in zepto_py/__init__.py")
    return parse_version(match.group(1))


def read_web_version(root: Path) -> tuple[int, int, int]:
    data = json.loads((root / "web" / "package.json").read_text(encoding="utf-8"))
    return parse_version(data["version"])


def read_chart_app_version(root: Path) -> tuple[int, int, int]:
    text = (root / "deploy" / "helm" / "zeptodb" / "Chart.yaml").read_text(
        encoding="utf-8"
    )
    match = re.search(r'^appVersion: "([^"]+)"$', text, flags=re.MULTILINE)
    if not match:
        raise ValueError("could not find appVersion in deploy/helm/zeptodb/Chart.yaml")
    return parse_version(match.group(1))


def current_file_versions(root: Path) -> list[tuple[int, int, int]]:
    return [
        read_cmake_version(root),
        read_python_version(root),
        read_web_version(root),
        read_chart_app_version(root),
    ]


def next_release_version(root: Path) -> str:
    tag_versions = current_git_tag_versions()
    file_version = max(current_file_versions(root))
    tag_version = max(tag_versions) if tag_versions else (0, 0, 0)
    if file_version > tag_version:
        return format_version(file_version)
    major, minor, patch = tag_version
    return format_version((major, minor, patch + 1))


def replace_once(path: Path, pattern: str, replacement: str) -> None:
    text = path.read_text(encoding="utf-8")
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"expected exactly one replacement in {path}")
    path.write_text(updated, encoding="utf-8")


def bump(root: Path, version: str, *, dry_run: bool) -> None:
    parse_version(version)
    targets = [
        root / "CMakeLists.txt",
        root / "zepto_py" / "__init__.py",
        root / "web" / "package.json",
        root / "deploy" / "helm" / "zeptodb" / "Chart.yaml",
        root / "deploy" / "docker" / "Dockerfile",
        root / "deploy" / "docker" / "Dockerfile.arm64",
    ]

    if dry_run:
        for target in targets:
            print(f"would update {target.relative_to(root)} to {version}")
        return

    replace_once(
        root / "CMakeLists.txt",
        r"(project\(\s*zeptodb\s*\n\s*VERSION\s+)\d+\.\d+\.\d+",
        rf"\g<1>{version}",
    )
    replace_once(
        root / "zepto_py" / "__init__.py",
        r'^__version__ = "[^"]+"$',
        f'__version__ = "{version}"',
    )

    package_path = root / "web" / "package.json"
    package_data = json.loads(package_path.read_text(encoding="utf-8"))
    package_data["version"] = version
    package_path.write_text(json.dumps(package_data, indent=2) + "\n", encoding="utf-8")
    replace_once(
        root / "deploy" / "helm" / "zeptodb" / "Chart.yaml",
        r'^appVersion: "[^"]+"$',
        f'appVersion: "{version}"',
    )
    for dockerfile in (
        root / "deploy" / "docker" / "Dockerfile",
        root / "deploy" / "docker" / "Dockerfile.arm64",
    ):
        replace_once(
            dockerfile,
            r"^ARG ZEPTO_VERSION=\d+\.\d+\.\d+$",
            f"ARG ZEPTO_VERSION={version}",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("next", help="print the next release version")

    bump_parser = subcommands.add_parser("bump", help="update version files")
    bump_parser.add_argument("version", help="MAJOR.MINOR.PATCH")
    bump_parser.add_argument("--dry-run", action="store_true")

    args = parser.parse_args()
    root = repo_root()

    if args.command == "next":
        print(next_release_version(root))
    elif args.command == "bump":
        bump(root, args.version, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

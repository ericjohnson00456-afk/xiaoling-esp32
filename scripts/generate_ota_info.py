#!/usr/bin/env python3
"""Generate the OTA metadata text file consumed by the release backend."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def parse_version(version: str) -> tuple[int, int, int]:
    parts = version.split(".")
    if len(parts) != 3:
        raise ValueError(f"invalid version '{version}', expected MAJOR.MINOR.PATCH")
    try:
        triplet = tuple(int(part) for part in parts)
    except ValueError as exc:
        raise ValueError(
            f"invalid version '{version}', expected MAJOR.MINOR.PATCH"
        ) from exc
    if any(component < 0 or component > 999 for component in triplet):
        raise ValueError(f"version component out of range in '{version}'")
    return triplet


def calculate_md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    major, minor, patch = parse_version(args.version)
    version_number = major * 1_000_000 + minor * 1_000 + patch
    content = (
        f"版本名: {args.version}\n"
        f"版本号: {version_number}\n"
        f"MD5值校验: {calculate_md5(args.app)}\n"
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
        args.output.write_text(content, encoding="utf-8")
    print(f"Generated OTA info: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

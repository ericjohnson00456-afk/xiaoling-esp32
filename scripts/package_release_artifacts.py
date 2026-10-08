#!/usr/bin/env python3
"""Create board-prefixed Xiaoling firmware artifacts and a release zip."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import tempfile
import zipfile
from pathlib import Path


_SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+$")


def _read_board_from_compile_commands(build_dir: Path) -> str:
    compile_commands = build_dir / "compile_commands.json"
    try:
        entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read {compile_commands}: {exc}") from exc

    # BOARD_NAME distinguishes concrete build variants. It defaults to
    # BOARD_TYPE in main/CMakeLists.txt, so it also works for normal builds.
    for define in ("BOARD_NAME", "BOARD_TYPE"):
        quoted = re.compile(rf'-D{define}=\\?"([^"\\]+)\\?"')
        unquoted = re.compile(rf"-D{define}=([^\s]+)")
        for entry in entries:
            command = entry.get("command") or " ".join(entry.get("arguments", []))
            match = quoted.search(command) or unquoted.search(command)
            if match:
                return match.group(1).strip('\\"')

    raise RuntimeError(
        f"cannot determine BOARD_NAME or BOARD_TYPE from {compile_commands}"
    )


def _validate_name(label: str, value: str) -> str:
    if not value or not _SAFE_NAME.fullmatch(value):
        raise ValueError(f"invalid {label}: {value!r}")
    return value


def _copy_release_files(
    build_dir: Path, board: str, project_name: str
) -> list[Path]:
    source_to_name = (
        (build_dir / "merged-binary.bin", f"{board}-merged-binary.bin"),
        (
            build_dir / f"{project_name}_ota_info.txt",
            f"{board}-{project_name}_ota_info.txt",
        ),
        (build_dir / f"{project_name}.bin", f"{board}-{project_name}.bin"),
        (build_dir / f"{project_name}.elf", f"{board}-{project_name}.elf"),
    )

    missing = [str(source) for source, _ in source_to_name if not source.is_file()]
    if missing:
        raise FileNotFoundError("missing build artifacts: " + ", ".join(missing))

    outputs: list[Path] = []
    for source, output_name in source_to_name:
        output = build_dir / output_name
        shutil.copy2(source, output)
        outputs.append(output)
    return outputs


def _write_zip(zip_path: Path, files: list[Path]) -> None:
    # Write beside the destination and replace it only after a complete archive
    # has been produced, so an interrupted build cannot leave a partial zip.
    with tempfile.NamedTemporaryFile(
        dir=zip_path.parent, prefix=f".{zip_path.name}.", suffix=".tmp", delete=False
    ) as temporary:
        temporary_path = Path(temporary.name)
    try:
        with zipfile.ZipFile(
            temporary_path, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for artifact in files:
                archive.write(artifact, arcname=artifact.name)
        temporary_path.replace(zip_path)
    finally:
        temporary_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--project-name", required=True)
    parser.add_argument("--board", default="")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    project_name = _validate_name("project name", args.project_name)
    board = args.board or _read_board_from_compile_commands(build_dir)
    board = _validate_name("board name", board)

    artifacts = _copy_release_files(build_dir, board, project_name)
    zip_path = build_dir / f"{board}-{project_name}.zip"
    _write_zip(zip_path, artifacts)

    print(f"Generated release package: {zip_path}")
    for artifact in artifacts:
        print(f"  {artifact.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

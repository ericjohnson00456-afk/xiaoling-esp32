"""Reuse the selected board's existing configuration for common miniapp code."""
import argparse
import json
from pathlib import Path


def generate(board_dir: Path, output: Path, boards_root: Path | None = None) -> None:
    config_path = board_dir / "config.json"
    config = json.loads(config_path.read_text()) if config_path.exists() else {}
    shape = config.get("display_shape", "rectangle")
    if shape not in ("rectangle", "round"):
        raise ValueError(f"Invalid display_shape for {board_dir.name}: {shape}")
    lines = ["// Generated from the selected board; do not edit.", "#pragma once"]
    if (board_dir / "config.h").exists():
        root = boards_root or Path(__file__).resolve().parents[1] / "main/boards"
        relative = board_dir.resolve().relative_to(root.resolve()).as_posix()
        lines.append(f'#include "boards/{relative}/config.h"')
    lines.append(f'#define MINIAPP_BOARD_DISPLAY_SHAPE "{shape}"')
    output.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.board_dir, args.output)

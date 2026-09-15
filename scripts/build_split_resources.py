#!/usr/bin/env python3
"""Build the independently flashable ARCS resource ROMFS images.

The runtime treats respak, tone, emoji and wake_word as separate partitions so
an OTA response can replace one resource without invalidating the others.
"""
import argparse
import json
import shutil
import tempfile
from pathlib import Path
from romfs import pack_image


def copy_tree(src: Path, dst: Path) -> None:
    if not src.is_dir():
        return
    for entry in src.iterdir():
        target = dst / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, dirs_exist_ok=True)
        elif entry.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(entry, target)


def package(root: Path, output: Path, name: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    pack_image(str(root), str(output), name)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--font-file")
    parser.add_argument("--wake-word-dir")
    parser.add_argument("--wake-word-file")
    parser.add_argument("--wake-word-name")
    args = parser.parse_args()

    source = Path(args.resource_root)
    output = Path(args.output_dir)
    with tempfile.TemporaryDirectory(prefix="split-res-") as temp:
        temp_root = Path(temp)
        respak = temp_root / "respak"
        tone = temp_root / "tone"
        emoji = temp_root / "emoji"
        wake = temp_root / "wake_word"
        for directory in (respak, tone, emoji, wake):
            directory.mkdir()

        # Static UI resources belong to respak. Tone and emoji are intentionally
        # excluded because they are updated through their own URLs.
        for entry in source.iterdir():
            if entry.name in ("tone", "emoji"):
                continue
            target = respak / entry.name
            if entry.is_dir():
                shutil.copytree(entry, target, dirs_exist_ok=True)
            elif entry.is_file():
                shutil.copy2(entry, target)

        if args.font_file:
            font = Path(args.font_file)
            if font.is_file():
                (respak / "font").mkdir(exist_ok=True)
                shutil.copy2(font, respak / "font" / font.name)

        copy_tree(source / "tone", tone)
        copy_tree(source / "emoji", emoji)
        if args.wake_word_dir:
            copy_tree(Path(args.wake_word_dir), wake)
        if args.wake_word_file:
            model = Path(args.wake_word_file)
            if model.is_file():
                shutil.copy2(model, wake / model.name)
        if args.wake_word_file and args.wake_word_name:
            (wake / "index.json").write_text(json.dumps({
                "model": Path(args.wake_word_file).name,
                "wake_word": args.wake_word_name,
            }, ensure_ascii=False), encoding="utf-8")

        package(respak, output / "respak.bin", "respak")
        if any(tone.iterdir()):
            package(tone, output / "tone.bin", "tone")
        if any(emoji.iterdir()):
            package(emoji, output / "emoji.bin", "emoji")
        if any(wake.iterdir()):
            package(wake, output / "wake_word.bin", "wake_word")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

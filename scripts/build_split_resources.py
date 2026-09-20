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


def relocate_activation_assets(locales: Path, destination: Path) -> None:
    """Move activation audio into the reserved 300-series ROMFS namespace."""
    if not locales.is_dir():
        return
    for language in locales.iterdir():
        if not language.is_dir():
            continue
        files = {"300_activation.ogg": language / "activation.ogg"}
        files.update({f"{301 + digit}_{digit}.ogg": language / f"{digit}.ogg"
                      for digit in range(10)})
        target = destination / language.name
        for name, source in files.items():
            if source.is_file():
                target.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target / name)
                source.unlink()


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

        # Static UI resources and localized activation audio belong to respak.
        # Tone and emoji are intentionally excluded because they are updated
        # through their own URLs. Keeping locales here makes activation audio
        # survive tone OTA updates.
        for entry in source.iterdir():
            if entry.name in ("tone", "emoji"):
                continue
            target = respak / entry.name
            if entry.is_dir():
                shutil.copytree(entry, target, dirs_exist_ok=True)
            elif entry.is_file():
                shutil.copy2(entry, target)

        relocate_activation_assets(respak / "locales", respak / "activation")

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
            manifest = {
                "model": Path(args.wake_word_file).name,
                "wake_word": args.wake_word_name,
            }
            model_descriptor = Path(args.wake_word_file).with_suffix(".json")
            if model_descriptor.is_file():
                try:
                    descriptor = json.loads(model_descriptor.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as exc:
                    raise RuntimeError(
                        f"Invalid wake-word descriptor {model_descriptor}: {exc}"
                    ) from exc
                if not isinstance(descriptor, dict):
                    raise RuntimeError(
                        f"Wake-word descriptor must contain a JSON object: {model_descriptor}"
                    )
                manifest = descriptor
                # The selected build configuration remains authoritative for
                # the file and wake-word fields consumed by the firmware.
                manifest["model"] = model.name
                manifest["wake_word"] = args.wake_word_name
            descriptor_path = wake / model_descriptor.name
            descriptor_path.write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )

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

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


PACKAGED_LANGUAGES = ("zh-CN", "en-US")


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


def copy_wake_word_bundle(bundle_dir: Path, destination: Path,
                          wake_word_name: str | None) -> None:
    """Copy exactly one matching TFLite/JSON pair into the ROMFS root."""
    if not bundle_dir.is_dir():
        raise RuntimeError(f"Wake-word bundle directory not found: {bundle_dir}")

    models = sorted(bundle_dir.glob("*.tflite"))
    descriptors = sorted(bundle_dir.glob("*.json"))
    if len(models) != 1 or len(descriptors) != 1:
        raise RuntimeError(
            f"Wake-word bundle {bundle_dir} must contain exactly one .tflite "
            f"and one .json file (found {len(models)} model(s), "
            f"{len(descriptors)} descriptor(s))"
        )

    model = models[0]
    descriptor_path = descriptors[0]
    if model.stem != descriptor_path.stem:
        raise RuntimeError(
            f"Wake-word model and descriptor names must match: "
            f"{model.name}, {descriptor_path.name}"
        )

    try:
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"Invalid wake-word descriptor {descriptor_path}: {exc}"
        ) from exc
    if not isinstance(descriptor, dict):
        raise RuntimeError(
            f"Wake-word descriptor must contain a JSON object: {descriptor_path}"
        )

    selected_name = wake_word_name or descriptor.get("wake_word")
    if not isinstance(selected_name, str) or not selected_name:
        raise RuntimeError(
            f"Wake-word descriptor is missing a valid wake_word: {descriptor_path}"
        )

    # The selected SDK configuration is authoritative. Only this model pair is
    # emitted, and both files are flattened into the wake_word ROMFS root.
    descriptor["model"] = model.name
    descriptor["wake_word"] = selected_name
    shutil.copy2(model, destination / model.name)
    (destination / descriptor_path.name).write_text(
        json.dumps(descriptor, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--font-file")
    wake_word_source = parser.add_mutually_exclusive_group()
    wake_word_source.add_argument("--wake-word-dir")
    wake_word_source.add_argument("--wake-word-file")
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

        # Static UI resources and localized activation audio belong to the
        # explicitly scoped respak directory. Keeping the four resource roots
        # separate prevents a new tone/emoji file from silently changing the
        # respak image.
        respak_source = source / "respak"
        if not respak_source.is_dir():
            raise RuntimeError(f"Missing respak resource directory: {respak_source}")
        copy_tree(respak_source, respak)
        # All flash sizes ship the same Chinese/English resource bundle.
        # Prune the temporary copy so other source locales remain available.
        locales = respak / "locales"
        for language in PACKAGED_LANGUAGES:
            if not (locales / language).is_dir():
                raise RuntimeError(f"Resource locale not found: {locales / language}")
        for locale in locales.iterdir():
            if locale.name not in PACKAGED_LANGUAGES:
                if locale.is_dir():
                    shutil.rmtree(locale)
                else:
                    locale.unlink()

        relocate_activation_assets(respak / "locales", respak / "activation")

        if args.font_file:
            font = Path(args.font_file)
            if font.is_file():
                (respak / "font").mkdir(exist_ok=True)
                shutil.copy2(font, respak / "font" / font.name)

        copy_tree(source / "tone", tone)
        copy_tree(source / "emoji", emoji)
        if args.wake_word_dir:
            copy_wake_word_bundle(Path(args.wake_word_dir), wake,
                                  args.wake_word_name)
        elif args.wake_word_file:
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
        # The partition is mounted even when this board has no wake model.
        package(wake, output / "wake_word.bin", "wake_word")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

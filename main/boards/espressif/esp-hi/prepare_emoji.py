#!/usr/bin/env python3
"""Prepare the reduced animation bundle for the ESP-HI 4MB profile."""
import argparse
import json
import shutil
from pathlib import Path


def copy_emoji_bundle(source: Path, destination: Path, step: int) -> None:
    manifest = json.loads((source / "emoji.json").read_text(encoding="utf-8"))
    for animation in manifest["animations"]:
        for phase in animation.values():
            if not isinstance(phase, dict) or "path" not in phase or "range" not in phase:
                continue
            begin, end = phase["range"]
            delay = phase.get("interval_ms", 100)
            delays = [delay] * (end - begin + 1)
            for index, value in phase.get("interval_overrides", []):
                local = index - begin if begin <= index <= end else index
                delays[local] = value
            pattern = phase["path"].lstrip("/")
            selected = list(range(begin, end + 1, step))
            intervals = []
            for new_index, old_index in enumerate(selected):
                src = source / (pattern % old_index)
                dst = destination / (pattern % new_index)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                intervals.append(sum(delays[old_index - begin:old_index - begin + step]))
            phase["range"] = [0, len(selected) - 1]
            phase["interval_ms"] = intervals[0]
            phase["interval_overrides"] = [[i, delay] for i, delay in enumerate(intervals)
                                           if delay != intervals[0]]
    (destination / "emoji.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--step", type=int, default=3)
    args = parser.parse_args()
    if args.step < 2:
        parser.error("ESP-HI animation thinning requires a step of at least two")
    source, output = args.source.resolve(), args.output.resolve()
    if output == source or output in source.parents or source in output.parents:
        parser.error("output must be separate from the source resource tree")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    copy_emoji_bundle(source, output, args.step)


if __name__ == "__main__":
    main()

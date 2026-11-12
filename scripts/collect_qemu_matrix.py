#!/usr/bin/env python3
"""Audit and combine matrix results produced in independent source worktrees."""
import argparse
import json
from pathlib import Path
import re

import qemu_matrix as matrix


def audit_memory_defaults(profile, config):
    files = [matrix.ROOT / "sdkconfig.defaults",
             matrix.ROOT / ("sdkconfig.defaults." + profile["target"]),
             matrix.ROOT / "sdkconfig.lsplatform",
             matrix.ROOT / "main/boards/common/sdkconfig.defaults",
             matrix.ROOT / ("main/boards/common/sdkconfig.defaults." + profile["target"]),
             matrix.ROOT / "main/boards" / profile["board"] / "sdkconfig.defaults"]
    entries = [line for path in files if path.exists() for line in path.read_text().splitlines()]
    entries += profile["append"]
    enabled = None
    selected = {}
    for line in entries:
        if line.startswith("CONFIG_SPIRAM="):
            enabled = line == "CONFIG_SPIRAM=y"
        elif line == "# CONFIG_SPIRAM is not set":
            enabled = False
        match = re.fullmatch(r"CONFIG_(SPIRAM_MODE|SPIRAM_SPEED|ESPTOOLPY_FLASHSIZE)_(\w+)=y", line)
        if match:
            selected[match[1]] = line.split("=")[0]
    if enabled is not None and (config.get("CONFIG_SPIRAM") == "y") != enabled:
        raise ValueError("production PSRAM enable differs from current board defaults")
    for group, symbol in selected.items():
        if group.startswith("SPIRAM") and config.get("CONFIG_SPIRAM") != "y":
            continue
        if config.get(symbol) != "y":
            raise ValueError(f"production memory setting differs from board defaults: {symbol}")


def audit(profile, work, result):
    for key in ("board", "name", "target", "symbol", "append"):
        if result.get(key) != profile[key]:
            raise ValueError(f"profile changed: {key}")
    if result.get("qemu", {}) != profile.get("qemu", {}):
        raise ValueError("board simulation memory profile changed")
    matrix.BUILD_SUBDIR = result.get("build_subdir", "build")
    matrix.validate_selection(profile, work, False)
    matrix.validate_selection(profile, work, True)
    audit_memory_defaults(profile, matrix.sdkconfig(work / "sdkconfig.original"))
    fixture_config = matrix.sdkconfig(work / "sdkconfig.fixture")
    for option in profile.get("qemu", {}).get("sdkconfig_append", []):
        symbol, value = option.split("=", 1)
        actual = fixture_config.get(symbol, "n")
        if actual != value:
            raise ValueError(f"board simulation setting not applied: {symbol}")
    for phase in ("original", "fixture"):
        record = result[phase]
        contains_fixture = b"QEMU_NETWORK_READY" in (work / phase / "app.bin").read_bytes()
        if contains_fixture != (phase == "fixture"):
            raise ValueError(f"hardware/test image boundary mismatch: {phase}")
        config = matrix.sdkconfig(work / ("sdkconfig.original" if phase == "original" else "sdkconfig.fixture"))
        if record["psram_enabled"] != matrix.memory_config(config)["psram_enabled"]:
            raise ValueError(f"PSRAM configuration mismatch: {phase}")
        for key, name in (("app_sha256", "app.bin"), ("elf_sha256", "app.elf"),
                          ("initial_flash_sha256", "flash.bin"),
                          ("partition_table_sha256", "partition-table.bin"),
                          ("dependency_lock_sha256", "dependencies.lock")):
            if matrix.sha(work / phase / name) != record[key]:
                raise ValueError(f"artifact hash mismatch: {phase}/{name}")
    fixture = result["fixture"]
    command = json.loads((work / "fixture/command.json").read_text())
    if "-snapshot" not in command:
        raise ValueError("fixture did not use a disposable flash snapshot")
    if profile["target"] == "esp32s3" and fixture["psram_enabled"]:
        capacity = profile.get("qemu", {}).get("psram_mb", 8)
        if "-m" not in command or command[command.index("-m") + 1] != f"{capacity}M":
            raise ValueError("simulated PSRAM capacity differs from board profile")
    evidence = (work / "fixture/evidence.log").read_text()
    raw = (work / "fixture/uart-private.log").read_text(errors="replace")
    if (fixture["status"] != "PASS" or not fixture["activation_code_received"]
            or not fixture["network_ready"] or not matrix.ACTIVATION.search(evidence)
            or "QEMU_NETWORK_READY" not in evidence or matrix.PANIC.search(raw)
            or fixture.get("observation_after_code_seconds", 0) < 2):
        raise ValueError("real-cloud activation evidence missing or failed")
    if fixture["psram_enabled"] and not fixture.get("psram_heap_at_main"):
        raise ValueError("required PSRAM heap missing")
    if profile["target"] == "esp32p4" and fixture["psram_enabled"]:
        if not fixture.get("p4_virtual_ram_probe"):
            raise ValueError("P4 RAM read/write probe missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", action="append", type=Path, required=True,
                        help="Matrix output; later audited PASS results take precedence")
    parser.add_argument("--output", type=Path, default=matrix.ROOT / "tmp/qemu-results")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inventory = matrix.profiles()
    matrix.write_json(output / "inventory.json", inventory)
    links = []
    for profile in inventory:
        selected = {**profile, "status": "NOT_RUN", "reason": "not_started"}
        rejected = []
        for source in args.source:
            work = source.resolve() / profile["name"]
            path = work / "result.json"
            if not path.exists():
                continue
            candidate = json.loads(path.read_text())
            if candidate.get("status") == "PASS":
                try:
                    audit(profile, work, candidate)
                except (ValueError, KeyError, OSError, RuntimeError) as error:
                    rejected.append({"artifact_directory": str(work), "reason": str(error)})
                    continue
                selected = {**candidate, "artifact_directory": str(work), "artifact_audit": "PASS"}
                for phase in ("original", "fixture"):
                    config = matrix.sdkconfig(work / ("sdkconfig.original" if phase == "original" else "sdkconfig.fixture"))
                    memory = matrix.memory_config(config)
                    selected[phase] = dict(candidate[phase])
                    if selected[phase].get("psram_mode") != memory["psram_mode"]:
                        selected[phase]["recorded_psram_mode_before_config_audit"] = selected[phase].get("psram_mode")
                    selected[phase].update(memory)
                    selected[phase]["chip_revision_min_full"] = config.get("CONFIG_ESP_REV_MIN_FULL")
                    selected[phase]["chip_revision_max_full"] = config.get("CONFIG_ESP_REV_MAX_FULL")
            elif selected["status"] != "PASS":
                selected = {**candidate, "artifact_directory": str(work)}
        if rejected:
            selected["rejected_results"] = rejected
            if selected["status"] == "NOT_RUN":
                selected.update(status="FAIL", reason="previous_PASS_failed_artifact_audit")
        destination = output / profile["name"]
        destination.mkdir(exist_ok=True)
        matrix.write_json(destination / "result.json", selected)
        if selected["status"] == "PASS":
            directory = Path(selected["artifact_directory"])
            links.append(f"| {profile['name']} | [正常镜像]({directory / 'original/flash.bin'}) | "
                         f"[激活证据]({directory / 'fixture/evidence.log'}) | "
                         f"[配置差异]({directory / 'fixture-config-overrides.json'}) |")
    results = matrix.report(output, inventory)
    report = output / "report.md"
    with report.open("a") as stream:
        stream.write("\n## 已审计产物\n\n正常镜像与测试镜像分开保存；正常镜像的外设运行结果见上表。\n\n")
        stream.write("| 变体 | 正常固件 | 测试证据 | 仿真配置 |\n| --- | --- | --- | --- |\n")
        stream.write("\n".join(links) + "\n")
    passed = sum(r["status"] == "PASS" for r in results)
    print(f"{passed}/{len(results)} audited virtual-board activation PASS; "
          f"{len({p['board'] for p in inventory})} board directories")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

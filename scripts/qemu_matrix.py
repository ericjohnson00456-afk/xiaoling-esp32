#!/usr/bin/env python3
"""Build every release profile and record original/virtual-board QEMU results.

Only a real activation response counts as PASS. A virtual-board PASS does not
validate the board's physical drivers, Wi-Fi, modem, display, camera or audio.
Run this script inside the repository's ESP-IDF environment.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timezone
from itertools import groupby
from functools import lru_cache

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import build as board_build


def idf_command():
    environment = os.environ.get("IDF_PYTHON_ENV_PATH")
    python = Path(environment) / "bin/python" if environment else Path(sys.executable)
    return [str(python), str(Path(os.environ["IDF_PATH"]) / "tools/idf.py")]


@lru_cache(maxsize=None)
def _idf_version(command):
    return subprocess.check_output(list(command) + ["--version"], text=True).strip()


def idf_version():
    return _idf_version(tuple(idf_command()))


def select_sdk(path):
    path = path.resolve()
    text = (path / "tools/cmake/version.cmake").read_text()
    major, minor, patch = [re.search(rf"set\(IDF_VERSION_{key} ([0-9]+)\)", text)[1]
                           for key in ["MAJOR", "MINOR", "PATCH"]]
    root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    candidates = []
    for base in (root, root.parent, Path.home() / ".espressif"):
        candidates += list(base.glob(f"python_env/idf{major}.{minor}_py*_env/bin/python"))
        candidates += list(base.glob(f"python/v{major}.{minor}.*/venv/bin/python"))
    if not candidates:
        raise RuntimeError(f"No installed Python environment for ESP-IDF {major}.{minor}")
    os.environ["IDF_PATH"] = str(path)
    os.environ["IDF_PYTHON_ENV_PATH"] = str(sorted(candidates)[-1].parent.parent)
    os.environ["PATH"] = str(sorted(candidates)[-1].parent) + os.pathsep + os.environ["PATH"]
    if root.name == "tools" and (root / "riscv32-esp-elf").exists():
        os.environ["IDF_TOOLS_PATH"] = str(root.parent)
    return f"idf{major}_{minor}_{patch}"


def add_installed_tool_paths():
    # An EIM environment originally installed for Xtensa can omit the already
    # installed RISC-V compiler from PATH. Use the SDK's exact recommended tools.
    info = json.loads((Path(os.environ["IDF_PATH"]) / "tools/tools.json").read_text())
    root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    paths = [str(Path(os.environ["IDF_PATH"]) / "tools")]
    wanted = {"xtensa-esp-elf", "riscv32-esp-elf", "xtensa-esp-elf-gdb", "riscv32-esp-elf-gdb"}
    for tool in info["tools"]:
        if tool["name"] not in wanted:
            continue
        version = next(v["name"] for v in tool["versions"] if v["status"] == "recommended")
        for base in (root, root / "tools"):
            for relative in tool["export_paths"]:
                path = base / tool["name"] / version / Path(*relative)
                if path.is_dir():
                    paths.append(str(path))
        if tool["name"].endswith("-gdb"):
            for base in (root, root / "tools"):
                for relative in tool["export_paths"]:
                    choices = sorted((base / tool["name"]).glob("*/" + "/".join(relative)))
                    if choices:
                        paths.append(str(choices[-1]))
    os.environ["PATH"] = os.pathsep.join(paths + [os.environ["PATH"]])

PANIC = re.compile(r"Guru Meditation|abort\(\) was called|assert failed|Rebooting\.\.\.")
ACTIVATION = re.compile(r"显示激活码: ([0-9]+), 二维码 URL: (https://\S+)")
BUILD_SUBDIR = "build"
MATRIX_STRIP_DEBUG = False
QEMU_BINARIES = {}
CONSOLE_CHOICES = {
    "CONFIG_ESP_CONSOLE_UART_DEFAULT", "CONFIG_ESP_CONSOLE_UART_CUSTOM",
    "CONFIG_ESP_CONSOLE_NONE", "CONFIG_ESP_CONSOLE_USB_CDC",
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG", "CONFIG_ESP_CONSOLE_SECONDARY_NONE",
    "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG",
}


def write_json(path, value):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temp.replace(path)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def progress(name, stage):
    print(f"{datetime.now().isoformat(timespec='seconds')} {name}: {stage}", flush=True)


def fingerprint(profile=None):
    paths = subprocess.check_output([
        "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
        "main", "components", "scripts", "CMakeLists.txt", "sdkconfig.defaults*", "sdkconfig.lsplatform",
    ], cwd=ROOT).decode().split("\0")
    digest = hashlib.sha256()
    for name in sorted(set(paths)):
        path = ROOT / name
        if profile:
            parts = Path(name).parts
            if len(parts) > 2 and parts[:2] == ("main", "boards"):
                if not any(name.startswith("main/boards/" + board + "/")
                           for board in (profile["board"], "common", "common_esp32s3")):
                    continue
                if parts[2] == "common_esp32s3" and profile["target"] != "esp32s3":
                    continue
            if name.startswith("sdkconfig.defaults.") and name != "sdkconfig.defaults." + profile["target"]:
                continue
        if name and path.is_file():
            digest.update(name.encode())
            digest.update(path.read_bytes())
    partition_files = list((ROOT / "partitions").rglob("*.csv"))
    if profile:
        selected = None
        defaults = [ROOT / "sdkconfig.defaults", ROOT / ("sdkconfig.defaults." + profile["target"]),
                    ROOT / "sdkconfig.lsplatform", ROOT / "main/boards/common/sdkconfig.defaults",
                    ROOT / ("main/boards/common/sdkconfig.defaults." + profile["target"]),
                    ROOT / "main/boards" / profile["board"] / "sdkconfig.defaults"]
        entries = [line for p in defaults if p.exists() for line in p.read_text().splitlines()]
        entries += profile["append"]
        for line in entries:
            if line.startswith("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="):
                selected = ROOT / line.split("=", 1)[1].strip('"')
        if selected and selected.exists():
            partition_files = [selected]
    for path in sorted(partition_files):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def profiles():
    mapping = board_build._parse_board_config_map()
    result = []
    boards_root = ROOT / "main/boards"
    for relative in sorted(set(mapping.values())):
        board = boards_root / relative
        configs = sorted(board.glob("config*.json"))
        if not configs:
            raise RuntimeError(f"Board missing release config: {relative}")
        memory_profile = board / "qemu.json"
        simulation_options = json.loads(memory_profile.read_text()) if memory_profile.exists() else {}
        for config in configs:
            data = json.loads(config.read_text())
            simulation = (simulation_options["targets"].get(data["target"], {})
                          if "targets" in simulation_options else simulation_options)
            for build in data["builds"]:
                symbol = build.get("board_type_config") or board_build._find_board_config(relative)
                if mapping.get(symbol) != relative:
                    raise RuntimeError(f"Invalid board selection: {relative}: {symbol}")
                result.append({"board": relative, "target": data["target"], "name": build["name"],
                               "config_file": str(config.relative_to(ROOT)), "symbol": symbol,
                               "append": list(build.get("sdkconfig_append", [])),
                               "qemu": simulation})
    if len({p["name"] for p in result}) != len(result):
        raise RuntimeError("Release variant names must be unique")
    return result


def report(output, inventory):
    results = []
    for profile in inventory:
        path = output / profile["name"] / "result.json"
        results.append(json.loads(path.read_text()) if path.exists() else
                       {**profile, "status": "NOT_RUN", "reason": "not_started"})
    write_json(output / "results.json", results)
    passed = sum(row["status"] == "PASS" for row in results)
    original_passed = sum(row.get("original", {}).get("status") == "PASS" for row in results)
    lines = ["# QEMU 激活验证矩阵", "",
             f"{len({p['board'] for p in inventory})} 个板型、{len(inventory)} 个发布变体；"
             f"虚拟板级边界测试通过 {passed} 项，原外设驱动镜像通过 {original_passed} 项。", "",
             "PASS 标准为取得真实云端激活码。虚拟测试替换板级外设和联网接入层，"
             "不代表真实 Wi-Fi/4G、显示、摄像头或音频已通过，不代表 WebSocket 已连接。", "",
             "| 板型 | 变体 | 芯片 | 原镜像 | 虚拟边界 | 原因 |",
             "| --- | --- | --- | --- | --- | --- |"]
    for row in results:
        original = row.get("original", {}).get("status", "NOT_RUN")
        reason = row.get("reason", "").replace("|", "/").replace("\n", " ")
        lines.append(f"| {row['board']} | {row['name']} | {row['target']} | {original} | {row['status']} | {reason} |")
    (output / "report.md").write_text("\n".join(lines) + "\n")
    return results


def sdkconfig(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines()
                if line.startswith("CONFIG_") and "=" in line)


def memory_config(config):
    enabled = config.get("CONFIG_SPIRAM") == "y"
    mode = "disabled"
    if enabled:
        mode = next((name.lower() for name in ("HEX", "OCT", "QUAD")
                     if config.get(f"CONFIG_SPIRAM_MODE_{name}") == "y"), "quad")
        if mode == "oct":
            mode = "octal"
    return {"psram_enabled": enabled, "psram_mode": mode,
            "psram_speed_mhz": int(config.get("CONFIG_SPIRAM_SPEED", "0")) if enabled else None}


def find_qemu(target):
    if target in QEMU_BINARIES:
        return QEMU_BINARIES[target]
    arch = "xtensa" if target in {"esp32", "esp32s3"} else "riscv32"
    root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    choices = list(root.glob(f"tools/qemu-{arch}/*/qemu/bin/qemu-system-{arch}"))
    choices += list(root.glob(f"qemu-{arch}/*/qemu/bin/qemu-system-{arch}"))
    if choices:
        return str(sorted(choices)[-1])
    binary = shutil.which(f"qemu-system-{arch}")
    if not binary:
        raise RuntimeError(f"Espressif QEMU not installed: {arch}")
    return binary


@lru_cache(maxsize=None)
def qemu_identity(path, timestamp):
    return {"qemu_binary": path, "qemu_binary_sha256": sha(Path(path)),
            "qemu_version": subprocess.check_output([path, "--version"], text=True).splitlines()[0]}


def run_logged(command, log, timeout=1800, cwd=ROOT, env=None):
    write_json(log.with_suffix(".command.json"), command)
    with log.open("w") as output:
        try:
            result = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=timeout, env=env)
            return result.returncode
        except subprocess.TimeoutExpired:
            return 124


def build_profile(profile, work, fixture):
    config = work / ("sdkconfig.fixture" if fixture else "sdkconfig.original")
    if fixture:
        lines = (work / "sdkconfig.original").read_text().splitlines()
        lines = [line for line in lines if not any(
            line.startswith(key + "=") or line == "# " + key + " is not set" for key in CONSOLE_CHOICES)]
        lines += ["CONFIG_ESP_CONSOLE_UART_DEFAULT=y", "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y",
                  "CONFIG_ETH_USE_OPENETH=y"]
        if profile["target"] in {"esp32c5", "esp32c6", "esp32p4"}:
            # The new QEMU MSPI models do not implement quad-enable commands.
            lines = [line for line in lines if not re.match(
                r"(?:# )?CONFIG_ESPTOOLPY_FLASHMODE_(?:QIO|QOUT|DIO|DOUT)(?:=| )", line)]
            lines.append("CONFIG_ESPTOOLPY_FLASHMODE_DIO=y")
        if profile["target"] == "esp32c5":
            # The current Espressif C5 machine has no PSRAM device. Keep the
            # production profile correct and test startup with internal RAM.
            lines = [line for line in lines if not line.startswith("CONFIG_SPIRAM=")]
            lines.append("# CONFIG_SPIRAM is not set")
        if profile["target"] == "esp32p4" and any(
                line == "CONFIG_SPIRAM=y" for line in lines):
            # QEMU exposes P4 external RAM as a normal mapped window, but its
            # current machine does not implement the firmware's PSRAM XIP
            # loader. Keep PSRAM available for heap allocations while running
            # code and read-only data from flash in the fixture.
            for key in ("CONFIG_SPIRAM_FETCH_INSTRUCTIONS",
                        "CONFIG_SPIRAM_RODATA",
                        "CONFIG_SPIRAM_XIP_FROM_PSRAM",
                        "CONFIG_SPIRAM_FLASH_LOAD_TO_PSRAM"):
                lines = [line for line in lines
                         if not (line.startswith(key + "=") or line == "# " + key + " is not set")]
                lines.append("# " + key + " is not set")
            for key in ("CONFIG_USE_AUDIO_PROCESSOR", "CONFIG_USE_DEVICE_AEC"):
                lines = [line for line in lines
                         if not (line.startswith(key + "=") or line == "# " + key + " is not set")]
                lines.append("# " + key + " is not set")
        if profile["target"] in {"esp32", "esp32s3"} and any(
                line == "CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y" for line in lines):
            # Espressif's ESP32 QEMU machine accepts up to 16MB flash images.
            # The SenseCAP 32MB layout has an equivalent 16MB fixture layout
            # for activation, so keep the production image unchanged and only
            # shrink this virtual test image.
            lines = [line for line in lines if not (
                line.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_") or
                line.startswith("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="))]
            lines += ["CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
                      'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m.csv"']
        fixture_options = profile.get("qemu", {}).get("sdkconfig_append", [])
        replaced = {line.split("=", 1)[0] for line in fixture_options}
        lines = [line for line in lines if not any(
            line.startswith(key + "=") or line == "# " + key + " is not set" for key in replaced)]
        lines += fixture_options
        config.write_text("\n".join(lines) + "\n")
    else:
        config.write_text("\n".join([f"{profile['symbol']}=y"] + profile["append"]) + "\n")
    identity = int(hashlib.sha256(profile["name"].encode()).hexdigest()[:8], 16)
    command = idf_command() + ["--ccache", "-B", str(work / BUILD_SUBDIR), f"-DSDKCONFIG={config}",
               f"-DIDF_TARGET={profile['target']}", f"-DBOARD_DIR={profile['board']}",
               f"-DBOARD_NAME={profile['name']}", "-DXIAOLING_DF_K10_QEMU=OFF",
               f"-DXIAOLING_MATRIX_STRIP_DEBUG={'ON' if MATRIX_STRIP_DEBUG else 'OFF'}",
               f"-DXIAOLING_QEMU={'ON' if fixture else 'OFF'}", f"-DXIAOLING_QEMU_ID={identity}", "build"]
    # Normalize only this build's generated paths, allowing identical IDF code
    # to share compiler cache entries across isolated board directories. Source
    # and preprocessed configuration contents remain part of the cache key.
    env = dict(os.environ, CCACHE_BASEDIR=str(work / BUILD_SUBDIR), CCACHE_NOHASHDIR="1")
    # EIM's optional offline snapshot may contain only a recent Wi-Fi remote
    # release, which cannot satisfy the project's pinned hosted version.
    if profile["target"] == "esp32p4":
        env.pop("IDF_COMPONENT_LOCAL_STORAGE_URL", None)
    return run_logged(command, work / ("build-fixture.log" if fixture else "build-original.log"), env=env)


def save_image(work, phase):
    build = work / BUILD_SUBDIR
    description = json.loads((build / "project_description.json").read_text())
    output = work / phase
    output.mkdir(exist_ok=True)
    app = build / (description["project_name"] + ".bin")
    elf = build / (description["project_name"] + ".elf")
    shutil.copyfile(app, output / "app.bin")
    shutil.copyfile(elf, output / "app.elf")
    shutil.copyfile(build / "merged-binary.bin", output / "flash.bin")
    shutil.copyfile(build / "partition_table/partition-table.bin", output / "partition-table.bin")
    shutil.copyfile(build / "bootloader/bootloader.bin", output / "bootloader.bin")
    shutil.copyfile(ROOT / "dependencies.lock", output / "dependencies.lock")
    config = sdkconfig(work / ("sdkconfig.fixture" if phase == "fixture" else "sdkconfig.original"))
    flash_mb = int(config["CONFIG_ESPTOOLPY_FLASHSIZE"].strip('"').removesuffix("MB"))
    flash = output / "flash.bin"
    with flash.open("ab") as stream:
        missing = flash_mb * 1024 * 1024 - flash.stat().st_size
        if missing < 0:
            raise RuntimeError("Merged image exceeds configured flash size")
        stream.write(b"\xff" * missing)
    return config, {"app_sha256": sha(output / "app.bin"), "elf_sha256": sha(output / "app.elf"),
                    "initial_flash_sha256": sha(flash), "dependency_lock_sha256": sha(output / "dependencies.lock"),
                    "partition_table_sha256": sha(output / "partition-table.bin"),
                    "app_bytes": app.stat().st_size, "flash_mb": flash_mb, **memory_config(config),
                    "chip_revision_min_full": config.get("CONFIG_ESP_REV_MIN_FULL"),
                    "chip_revision_max_full": config.get("CONFIG_ESP_REV_MAX_FULL")}


def validate_selection(profile, work, fixture):
    config = sdkconfig(work / ("sdkconfig.fixture" if fixture else "sdkconfig.original"))
    if config.get(profile["symbol"]) != "y":
        raise RuntimeError(f"Requested board choice was not selected: {profile['symbol']}")
    if config.get("CONFIG_IDF_TARGET", "").strip('"') != profile["target"]:
        raise RuntimeError("Generated target differs from the release profile")
    commands = json.loads((work / BUILD_SUBDIR / "compile_commands.json").read_text())
    entry = next(c for c in commands if c["file"].endswith("/main/main.cc"))
    for macro, expected in (("BOARD_TYPE", profile["board"]), ("BOARD_NAME", profile["name"])):
        match = re.search(rf'-D{macro}=\\?"([^"\\]+)\\?"', entry["command"])
        if not match or match[1] != expected:
            raise RuntimeError(f"Compiled {macro} differs from the release profile")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def qemu_command(profile, output, config, port):
    target = profile["target"]
    qemu = find_qemu(target)
    command = [qemu, "-machine", target, "-snapshot", "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-drive", f"file={output / 'flash.bin'},if=mtd,format=raw",
               "-nic", "user,model=open_eth", "-gdb", f"tcp:127.0.0.1:{port}"]
    timer = "timer.esp32.timg" if target == "esp32" else "timer.esp.timg"
    command += ["-global", f"driver={timer},property=wdt_disable,value=true"]
    if target in {"esp32s3", "esp32c6", "esp32p4"}:
        efuse = bytearray(1024)
        if target == "esp32s3":
            efuse[64] = 1  # BLOCK2 ADC calibration version
        elif target == "esp32c6":
            efuse[39] = 1 << 3  # C6 BLOCK1 bit 123: calibration block v0.1
        else:
            efuse[32] = 0x31  # P4 BLOCK1: silicon revision v3.1
            efuse[33] = 2  # calibration block v0.2, avoiding ADC self-calibration
        (output / "efuse.bin").write_bytes(efuse)
        command += ["-drive", f"file={output / 'efuse.bin'},if=none,format=raw,id=efuse",
                    "-global", f"driver=nvram.{target}.efuse,property=drive,value=efuse"]
        if target == "esp32s3" and config.get("CONFIG_SPIRAM") == "y":
            # Capacity is explicit simulation input, not a physical-board measurement.
            capacity = profile.get("qemu", {}).get("psram_mb", 8)
            command += ["-m", f"{capacity}M"]
            if config.get("CONFIG_SPIRAM_MODE_OCT") == "y":
                command += ["-global", "driver=ssi_psram,property=is_octal,value=true"]
    elif target == "esp32" and config.get("CONFIG_SPIRAM") == "y":
        command += ["-m", "4M"]
    return command


def diagnostic(profile, output, port):
    prefix = "xtensa-esp32s3" if profile["target"] == "esp32s3" else (
        "xtensa-esp32" if profile["target"] == "esp32" else "riscv32-esp")
    gdb = shutil.which(prefix + "-elf-gdb")
    if gdb:
        run_logged([gdb, "-batch", str(output / "app.elf"), "-ex", f"target remote 127.0.0.1:{port}",
                    "-ex", "thread apply all bt", "-ex", "detach"], output / "gdb.log", timeout=10)


def simulate(profile, work, phase, config, image, timeout):
    output = work / phase
    port = free_port()
    command = qemu_command(profile, output, config, port)
    write_json(output / "command.json", command)
    result = {**image, "scope": "original_hardware_drivers" if phase == "original" else "virtual_board_boundary",
              "status": "FAIL", "reason": "timeout", "activation_code_received": False,
              "network_ready": False, "app_main": False, "physical_board_verified": False,
              "websocket_verified": False}
    result.update(qemu_identity(command[0], Path(command[0]).stat().st_mtime_ns))
    started = time.monotonic()
    activation_at = None
    raw = output / "uart-private.log"
    fd = os.open(raw, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        try:
            while time.monotonic() - started < timeout:
                text = raw.read_text(errors="replace")
                result["app_main"] = "app_main entry heap:" in text
                result["network_ready"] = "QEMU_NETWORK_READY" in text or "Connected to WiFi:" in text
                activation = ACTIVATION.search(text)
                result["activation_code_received"] = activation is not None
                result["websocket_verified"] = "QEMU_CLOUD_CONNECTED" in text
                result["p4_virtual_ram_probe"] = "QEMU_P4_PSRAM_WINDOW_OK" in text
                heap = re.search(r"app_main entry heap:.*psram=([0-9]+)", text)
                result["psram_heap_at_main"] = int(heap[1]) if heap else None
                if PANIC.search(text):
                    result["reason"] = "guest_panic_or_reset"
                    break
                if activation and result["network_ready"]:
                    memory_ok = not image["psram_enabled"] or bool(result["psram_heap_at_main"])
                    if profile["target"] == "esp32p4" and image["psram_enabled"]:
                        memory_ok = memory_ok and result["p4_virtual_ram_probe"]
                    if memory_ok:
                        if activation_at is None:
                            activation_at = time.monotonic()
                        if time.monotonic() - activation_at >= 2:
                            result.update(status="PASS", reason="real_cloud_activation_code_received",
                                          observation_after_code_seconds=2)
                            break
                    else:
                        result["reason"] = "required_psram_unavailable"
                        break
                if process.poll() is not None:
                    result["reason"] = f"qemu_exit_{process.returncode}"
                    break
                time.sleep(0.2)
            if result["status"] != "PASS" and process.poll() is None:
                diagnostic(profile, output, port)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if process.stdin:
                process.stdin.close()
    result["elapsed_seconds"] = round(time.monotonic() - started, 2)
    text = raw.read_text(errors="replace")
    safe = [line for line in text.splitlines() if any(m in line for m in [
        "app_main entry heap:", "PROFILE=", "QEMU_NETWORK_READY", "显示激活码:", "Found ",
        "esp_psram: Speed:", "SNTP 首次校时完成", "QEMU_CLOUD_CONNECTED", "Guru Meditation",
        "abort() was called", "assert failed", "Backtrace:", "QEMU_P4_PSRAM_WINDOW_OK",
        "QEMU_ALLOC_FAILED"])]
    (output / "evidence.log").write_text("\n".join(safe) + "\n")
    write_json(output / "result.json", result)
    return result


def verify(profile, output, args, source_hash):
    work = output / profile["name"]
    work.mkdir(parents=True, exist_ok=True)
    result_path = work / "result.json"
    if result_path.exists() and args.resume:
        previous = json.loads(result_path.read_text())
        active_idf = idf_version()
        active_qemu = find_qemu(profile["target"])
        identity = qemu_identity(active_qemu, Path(active_qemu).stat().st_mtime_ns)
        if (previous.get("source_fingerprint") == source_hash and previous.get("status") == "PASS"
                and previous.get("idf_version") == active_idf
                and previous.get("fixture", {}).get("qemu_binary_sha256") == identity["qemu_binary_sha256"]):
            progress(profile["name"], "resume PASS")
            return previous
    result = {**profile, "source_fingerprint": source_hash, "status": "FAIL",
              "physical_board_verified": False, "websocket_verified": False,
              "idf_version": idf_version(),
              "build_subdir": BUILD_SUBDIR, "debug_info_stripped": MATRIX_STRIP_DEBUG}
    if not args.fixture_only:
        progress(profile["name"], "build original")
        code = build_profile(profile, work, False)
        if code:
            result["reason"] = f"original_build_exit_{code}"
            write_json(result_path, result)
            return result
        validate_selection(profile, work, False)
        config, image = save_image(work, "original")
        progress(profile["name"], "run original")
        result["original"] = simulate(profile, work, "original", config, image, args.original_timeout)
        write_json(result_path, result)
    elif not (work / "sdkconfig.original").exists():
        raise RuntimeError("--fixture-only requires the profile's original sdkconfig")
    else:
        if result_path.exists():
            previous = json.loads(result_path.read_text())
            if "original" in previous:
                result["original"] = previous["original"]
    progress(profile["name"], "build fixture")
    code = build_profile(profile, work, True)
    if code:
        result["reason"] = f"fixture_build_exit_{code}"
        write_json(result_path, result)
        return result
    validate_selection(profile, work, True)
    config, image = save_image(work, "fixture")
    original_config = sdkconfig(work / "sdkconfig.original")
    write_json(work / "fixture-config-overrides.json", {
        "ethernet": "OpenCores instead of Wi-Fi/4G/hosted Wi-Fi",
        "console": "UART0 instead of the hardware console",
        "flash_mode": config.get("CONFIG_ESPTOOLPY_FLASHMODE"),
        "production_psram": original_config.get("CONFIG_SPIRAM") == "y",
        "simulation_psram": config.get("CONFIG_SPIRAM") == "y",
        "c5_psram_not_emulated": profile["target"] == "esp32c5",
        "p4_psram_chip_init_virtualized": profile["target"] == "esp32p4" and config.get("CONFIG_SPIRAM") == "y",
        "board_fixture_options": profile.get("qemu", {}).get("sdkconfig_append", []),
    })
    progress(profile["name"], "run fixture")
    result["fixture"] = simulate(profile, work, "fixture", config, image, args.timeout)
    result["status"] = result["fixture"]["status"]
    result["reason"] = result["fixture"]["reason"]
    result["validated_scope"] = "virtual_board_boundary"
    result["original_firmware_qemu_pass"] = result.get("original", {}).get("status") == "PASS"
    write_json(result_path, result)
    progress(profile["name"], result["status"] + " " + result["reason"])
    return result


def verify_worker(profile, output, args, build_subdir):
    global BUILD_SUBDIR, MATRIX_STRIP_DEBUG, QEMU_BINARIES
    BUILD_SUBDIR = build_subdir
    MATRIX_STRIP_DEBUG = args.strip_debug
    QEMU_BINARIES = dict(item.split("=", 1) for item in args.qemu_for)
    try:
        return verify(profile, output, args, fingerprint(profile))
    except Exception as error:
        result = {**profile, "source_fingerprint": fingerprint(profile), "status": "FAIL",
                  "reason": f"{type(error).__name__}: {error}", "physical_board_verified": False}
        path = output / profile["name"]
        path.mkdir(parents=True, exist_ok=True)
        write_json(path / "result.json", result)
        progress(profile["name"], result["reason"])
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/qemu-matrix")
    parser.add_argument("--board", action="append")
    parser.add_argument("--target", action="append")
    parser.add_argument("--name", action="append")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--fixture-only", action="store_true")
    parser.add_argument("--report-only", action="store_true")
    parser.add_argument("--build-suffix", default="", help="Isolate builds made with a different SDK")
    parser.add_argument("--strip-debug", action="store_true",
                        help="Keep function symbols but omit DWARF from matrix ELF files")
    parser.add_argument("--idf-for", action="append", default=[], metavar="TARGET=PATH",
                        help="Use another installed SDK for a particular chip family")
    parser.add_argument("--qemu-for", action="append", default=[], metavar="TARGET=PATH",
                        help="Use a specific Espressif QEMU binary for one chip family")
    parser.add_argument("--jobs", type=int, default=1,
                        help="Concurrent profiles within one chip family (default 1)")
    parser.add_argument("--timeout", type=int, default=100)
    parser.add_argument("--original-timeout", type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    global BUILD_SUBDIR, MATRIX_STRIP_DEBUG
    MATRIX_STRIP_DEBUG = args.strip_debug
    if args.build_suffix and not re.fullmatch(r"[A-Za-z0-9_-]+", args.build_suffix):
        parser.error("Invalid --build-suffix")
    BUILD_SUBDIR = "build" + ("-" + args.build_suffix if args.build_suffix else "")
    os.chdir(ROOT)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    all_profiles = profiles()
    write_json(output / "inventory.json", all_profiles)
    if args.report_only:
        results = report(output, all_profiles)
        print(f"{sum(r['status'] == 'PASS' for r in results)}/{len(results)} fixture PASS")
        return 0
    add_installed_tool_paths()
    sdk_overrides = {}
    for entry in args.idf_for:
        target, separator, path = entry.partition("=")
        if not separator or target not in {p["target"] for p in all_profiles}:
            parser.error("--idf-for must be TARGET=PATH")
        sdk_overrides[target] = Path(path)
    for entry in args.qemu_for:
        target, separator, path = entry.partition("=")
        if not separator or target not in {p["target"] for p in all_profiles} or not Path(path).is_file():
            parser.error("--qemu-for must be TARGET=EXECUTABLE")
    baseline_environment = {key: os.environ.get(key) for key in
                            ["IDF_PATH", "IDF_PYTHON_ENV_PATH", "IDF_TOOLS_PATH", "PATH"]}
    baseline_build_subdir = BUILD_SUBDIR
    if args.board:
        args.board = [board_build.resolve_board(value) for value in args.board]
    selected = [p for p in all_profiles if
                (not args.board or p["board"] in args.board) and
                (not args.name or p["name"] in args.name) and
                (not args.target or p["target"] in args.target)]
    target_order = {target: index for index, target in enumerate(
        ["esp32s3", "esp32c3", "esp32c6", "esp32", "esp32c5", "esp32p4"])}
    selected.sort(key=lambda p: (target_order[p["target"]], p["name"]))
    if not selected:
        parser.error("No matching board profiles")
    source_hash = fingerprint()
    write_json(output / "session.json", {
        "started_utc": datetime.now(timezone.utc).isoformat(), "source_fingerprint": source_hash,
        "idf_version": idf_version(),
        "profiles_selected": len(selected), "profiles_total": len(all_profiles),
        "criterion": "real_cloud_activation_code_received", "physical_board_verified": False})
    results = []
    for target, group in groupby(selected, key=lambda p: p["target"]):
        for key, value in baseline_environment.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        BUILD_SUBDIR = baseline_build_subdir
        if target in sdk_overrides:
            BUILD_SUBDIR += "-" + select_sdk(sdk_overrides[target])
        add_installed_tool_paths()
        group = list(group)
        # Resolve/download a target's dependencies once before parallel builds.
        # Different chip families never mutate managed_components concurrently.
        results.append(verify_worker(group[0], output, args, BUILD_SUBDIR))
        report(output, all_profiles)
        if args.jobs == 1:
            for profile in group[1:]:
                results.append(verify_worker(profile, output, args, BUILD_SUBDIR))
                report(output, all_profiles)
        elif len(group) > 1:
            with ProcessPoolExecutor(max_workers=args.jobs) as executor:
                pending = [executor.submit(verify_worker, p, output, args, BUILD_SUBDIR) for p in group[1:]]
                for future in as_completed(pending):
                    results.append(future.result())
                    report(output, all_profiles)
    passed = sum(r["status"] == "PASS" for r in results)
    progress("matrix", f"{passed}/{len(results)} fixture profiles PASS")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

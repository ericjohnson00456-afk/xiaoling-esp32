#!/usr/bin/env python3
"""Run the DF-K10 cloud fixture; PASS never implies a physical-board pass."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
CONNECTED = "DF_K10_QEMU_CLOUD_CONNECTED"
DISCONNECTED = "DF_K10_QEMU_CLOUD_DISCONNECTED"


def run(command, log, **kwargs):
    with log.open("w") as output:
        subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                       check=True, **kwargs)


def find_qemu():
    override = shutil.which("qemu-system-xtensa")
    if override:
        return override
    root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    candidates = sorted(list(root.glob("qemu-xtensa/*/qemu/bin/qemu-system-xtensa")) +
                        list(root.glob("tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa")))
    if not candidates:
        raise RuntimeError("Install Espressif QEMU or pass --qemu")
    return str(candidates[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", help="Espressif qemu-system-xtensa executable")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--reuse-flash", action="store_true",
                        help="Keep simulated NVS/identity; requires the same app SHA256")
    parser.add_argument("--criterion", choices=["cloud", "activation"], default="cloud",
                        help="cloud requires WebSocket; activation only requires a binding code")
    parser.add_argument("--gdb-port", type=int, help="Optional localhost GDB port for diagnosis")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--hold", type=int, default=20,
                        help="Observe the connection for this many wall seconds")
    args = parser.parse_args()
    if args.timeout <= 0 or args.hold <= 0:
        parser.error("--timeout and --hold must be positive")
    os.chdir(ROOT)
    output = ROOT / "tmp/qemu-validation/df-k10-qemu"
    output.mkdir(parents=True, exist_ok=True)
    build = ROOT / "build-df-k10-qemu"
    config = output / "sdkconfig"
    if not args.skip_build:
        # A separate SDKCONFIG and build directory protect the customer image.
        if not config.exists():
            config.write_text("CONFIG_BOARD_TYPE_DF_K10=y\n"
                              "CONFIG_SPIRAM_MODE_OCT=y\n"
                              "CONFIG_ESP_CONSOLE_UART_DEFAULT=y\n"
                              "CONFIG_ETH_USE_OPENETH=y\n")
        run(["idf.py", "--ccache", "-B", str(build),
             f"-DSDKCONFIG={config}", "-DIDF_TARGET=esp32s3",
             "-DBOARD_TYPE=df-k10", "-DBOARD_NAME=df-k10",
             "-DXIAOLING_DF_K10_QEMU=ON", "build"],
            output / "build.log")

    commands = json.loads((build / "compile_commands.json").read_text())
    board_command = next(c["command"] for c in commands
                         if c["file"].endswith("/df-k10/df_k10_board.cc"))
    if "-DXIAOLING_DF_K10_QEMU=1" not in board_command:
        raise RuntimeError("Refusing to run a non-fixture build as a cloud fixture")
    description = json.loads((build / "project_description.json").read_text())
    app = build / (description["project_name"] + ".bin")
    app_sha256 = hashlib.sha256(app.read_bytes()).hexdigest()
    flash = output / "flash.bin"
    if args.reuse_flash:
        previous = json.loads((output / "result.json").read_text())
        if not flash.is_file() or previous["app_sha256"] != app_sha256:
            raise RuntimeError("--reuse-flash requires an existing image of the same application")
    else:
        run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
             "--fill-flash-size", "16MB", "-o", str(flash), "@flash_args"],
            output / "merge.log", cwd=build)

    # Simulated eFuses only. A valid calibration version avoids the SDK's ADC
    # self-calibration, whose ADC hardware is absent from QEMU. No GDB patches.
    efuse = output / "efuse.bin"
    efuse_data = bytearray(1024)
    # QEMU stores ESPEfuseBlocks, starting at BLOCK0 (not the register
    # address space used by espefuse --virt). BLOCK0/1 are 24 bytes each;
    # BLK_VERSION_MAJOR occupies bit 128 of BLOCK2. See esp_efuse_table.csv
    # and Espressif QEMU include/hw/nvram/esp_efuse.h.
    efuse_data[24 + 24 + 128 // 8] = 1
    efuse.write_bytes(efuse_data)

    qemu = args.qemu or find_qemu()
    version = subprocess.check_output([qemu, "--version"], text=True).splitlines()[0]
    result = {
        "board": "df-k10", "target": "esp32s3", "scope": "cloud_fixture",
        "criterion": args.criterion,
        "physical_board_verified": False, "original_firmware_qemu_pass": False,
        "qemu": version, "app_sha256": app_sha256,
        "qemu_binary": str(Path(qemu).resolve()),
        "qemu_binary_sha256": hashlib.sha256(Path(qemu).read_bytes()).hexdigest(),
        "virtual_ethernet": False, "token_received": False,
        "websocket_transport": False, "cloud_connected_event": False,
        "activation_required": False,
        "activation_code_received": False,
        "octal_psram_8mb_80mhz": False,
        "hold_seconds": args.hold if args.criterion == "cloud" else 0,
        "status": "FAIL", "reason": "timeout",
    }
    command = [qemu, "-machine", "esp32s3", "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-m", "8M", "-global", "driver=ssi_psram,property=is_octal,value=true",
               "-drive", f"file={flash},if=mtd,format=raw",
               "-drive", f"file={efuse},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
               "-nic", "user,model=open_eth"]
    if args.gdb_port:
        command += ["-gdb", f"tcp:127.0.0.1:{args.gdb_port}"]
    (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    uart = output / "uart-private.log"
    # The application logs authentication responses. Keep the raw log private;
    # evidence.log below contains only safe stage markers, without credentials.
    fd = os.open(uart, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    started = time.monotonic()
    connected_at = None
    try:
        with os.fdopen(fd, "w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.PIPE)
            try:
                while time.monotonic() - started < args.timeout:
                    text = uart.read_text(errors="replace")
                    result["virtual_ethernet"] = "Virtual Ethernet acquired IPv4" in text
                    result["token_received"] = bool(re.search(r"token_len=[1-9][0-9]*", text))
                    result["websocket_transport"] = "WebSocket transport connected" in text
                    result["cloud_connected_event"] = CONNECTED in text
                    result["activation_code_received"] = bool(re.search(r"显示激活码: [0-9]+, 二维码 URL: https://", text))
                    result["activation_required"] = (result["activation_code_received"] or "等待设备绑定" in text) and CONNECTED not in text
                    result["octal_psram_8mb_80mhz"] = all(m in text for m in [
                        "octal_psram:", "esp_psram: Found 8MB PSRAM device", "esp_psram: Speed: 80MHz"])
                    if re.search(r"Guru Meditation|abort\(\) was called|assert failed|Rebooting\.\.\.", text):
                        result["reason"] = "guest_panic_or_reset"
                        break
                    if args.criterion == "activation" and all(result[k] for k in [
                            "virtual_ethernet", "activation_code_received", "octal_psram_8mb_80mhz"]):
                        result.update(status="PASS", reason="real_cloud_activation_code_received")
                        break
                    if CONNECTED in text:
                        tail = text.split(CONNECTED, 1)[1]
                        if DISCONNECTED in tail or "WebSocket transport disconnected" in tail:
                            result["reason"] = "cloud_disconnected_during_observation"
                            break
                        if connected_at is None:
                            connected_at = time.monotonic()
                        if time.monotonic() - connected_at >= args.hold and all(
                                result[k] for k in ["virtual_ethernet", "token_received",
                                                   "websocket_transport", "cloud_connected_event",
                                                   "octal_psram_8mb_80mhz"]):
                            result.update(status="PASS", reason="real_cloud_connected_and_observed")
                            break
                    if process.poll() is not None:
                        result["reason"] = f"qemu_exit_{process.returncode}"
                        break
                    time.sleep(0.2)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                if process.stdin is not None:
                    process.stdin.close()
    finally:
        if result["reason"] == "timeout" and result["activation_required"]:
            result["reason"] = "cloud_requires_device_binding"
        result["elapsed_seconds"] = round(time.monotonic() - started, 2)
        text = uart.read_text(errors="replace")
        markers = ["TEST FIXTURE:", "Virtual Ethernet acquired IPv4", "Token 响应: transport_ok=",
                   "WebSocket transport connected", "显示激活码:", "octal_psram: density",
                   "Found 8MB PSRAM device", "esp_psram: Speed:", CONNECTED, DISCONNECTED]
        evidence = [line for line in text.splitlines() if any(m in line for m in markers)]
        (output / "evidence.log").write_text("\n".join(evidence) + "\n")
        (output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

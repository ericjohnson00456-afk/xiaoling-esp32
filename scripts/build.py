#!/usr/bin/env python3
"""Build board release variants from main/boards/**/config.json."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
_BOARDS_DIR = ROOT / "main/boards"
_NAME = re.compile(r"^[a-z0-9][a-z0-9._-]*$")


def get_project_version() -> str:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([^"\n]+)"\)', cmake)
    if not match:
        raise ValueError("CMakeLists.txt 未定义 PROJECT_VER")
    return match[1]


def _parse_board_config_map() -> dict[str, str]:
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    return dict(re.findall(
        r'(?:else)?if\((CONFIG_BOARD_TYPE_\w+)\)\s+'
        r'set\(BOARD_(?:DIR|TYPE)\s+"([^"]+)"\)', cmake))


def _find_board_config(board: str) -> str | None:
    symbols = [key for key, path in _parse_board_config_map().items() if path == board]
    return symbols[0] if symbols else None


def _safe_board_path(board: str) -> Path:
    path = Path(board)
    if path.is_absolute() or any(part in (".", "..") or not _NAME.fullmatch(part)
                                 for part in path.parts):
        raise ValueError(f"无效板型目录: {board!r}")
    resolved = (_BOARDS_DIR / path).resolve()
    if not resolved.is_relative_to(_BOARDS_DIR.resolve()):
        raise ValueError(f"板型目录超出 main/boards: {board!r}")
    return resolved


def _load_board(board: str, config_filename: str = "config.json") -> dict:
    if Path(config_filename).name != config_filename:
        raise ValueError("配置文件必须是板型目录内的文件名")
    path = _safe_board_path(board) / config_filename
    config = json.loads(path.read_text(encoding="utf-8"))
    reported = config.get("type", board.replace("/", "-"))
    if not _NAME.fullmatch(reported):
        raise ValueError(f"无效板型身份: {path}")
    manufacturer = config.get("manufacturer", "")
    if manufacturer and not _NAME.fullmatch(manufacturer):
        raise ValueError(f"无效厂商: {path}")
    if "/" in board and manufacturer != board.split("/")[0]:
        raise ValueError(f"厂商与目录不一致: {path}")
    if not re.fullmatch(r"esp32[a-z0-9]*", config.get("target", "")):
        raise ValueError(f"缺少或无效芯片目标: {path}")
    builds = config.get("builds")
    if not isinstance(builds, list) or not builds:
        raise ValueError(f"缺少发布变体: {path}")
    return {**config, "type": reported, "manufacturer": manufacturer}


def _board_symbol(board: str, config: dict, variant: dict) -> str:
    mapping = _parse_board_config_map()
    explicit = variant.get("board_type_config")
    selected = [line.partition("=")[0] for line in variant.get("sdkconfig_append", [])
                if line.endswith("=y") and line.partition("=")[0] in mapping]
    symbol = explicit or (selected[0] if len(selected) == 1 else _find_board_config(board))
    if not symbol or mapping.get(symbol) != board or any(key != symbol for key in selected):
        raise ValueError(f"板型选择与目录不匹配: {board}: {variant.get('name')}")
    kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
    match = re.search(r"\bconfig\s+" + re.escape(symbol.removeprefix("CONFIG_")) +
                      r"\b(.*?)(?=\n\s*(?:config|endchoice|endif)\b|\Z)",
                      kconfig, re.S)
    if not match:
        raise ValueError(f"Kconfig 中未定义 {symbol}")
    targets = set(re.findall(r"\bIDF_TARGET_(ESP32[A-Z0-9]*)\b", match[1]))
    if targets and config["target"].upper() not in targets:
        raise ValueError(f"{symbol} 不支持芯片 {config['target']}")
    return symbol


def _collect_variants(config_filename: str = "config.json") -> list[dict[str, str]]:
    variants = []
    types: dict[str, str] = {}
    for path in sorted(_BOARDS_DIR.rglob(config_filename)):
        relative = path.parent.relative_to(_BOARDS_DIR)
        if relative.parts[0] in ("common", "common_esp32s3"):
            continue
        board = relative.as_posix()
        config = _load_board(board, config_filename)
        previous = types.setdefault(config["type"], board)
        if previous != board:
            raise ValueError(f"板型身份重复: {config['type']}: {previous}, {board}")
        for variant in config["builds"]:
            name = variant.get("name", "")
            if not _NAME.fullmatch(name):
                raise ValueError(f"无效发布变体名称: {path}: {name!r}")
            _board_symbol(board, config, variant)
            variants.append({"board": board, "name": name, "target": config["target"]})
    if len({value["name"] for value in variants}) != len(variants):
        raise ValueError("发布变体名称必须唯一")
    return variants


def resolve_board(value: str, config_filename: str = "config.json") -> str:
    if (_safe_board_path(value) / config_filename).is_file():
        return value
    boards = {item["board"] for item in _collect_variants(config_filename)}
    matches = [board for board in boards
               if _load_board(board, config_filename)["type"] == value]
    if len(matches) != 1:
        raise ValueError(f"未知板型目录或身份: {value}")
    return matches[0]


def _board_type_exists(board: str) -> bool:
    try:
        resolve_board(board)
        return True
    except (ValueError, FileNotFoundError):
        return False


def _idf_command() -> list[str]:
    sdk = os.environ.get("IDF_PATH")
    if not sdk:
        raise RuntimeError("请先加载 ESP-IDF 6.1 环境")
    return [sys.executable, str(Path(sdk) / "tools/idf.py")]


def _sdkconfig(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text().splitlines()
                if line.startswith("CONFIG_") and "=" in line)


def _validate_selection(board: str, variant: dict, build_dir: Path,
                        config_filename: str = "config.json") -> None:
    config = _load_board(board, config_filename)
    symbol = _board_symbol(board, config, variant)
    actual = _sdkconfig(build_dir / "sdkconfig")
    selected = [key for key in _parse_board_config_map() if actual.get(key) == "y"]
    if selected != [symbol] or actual.get("CONFIG_IDF_TARGET", "").strip('"') != config["target"]:
        raise RuntimeError(f"生成配置未选择预期板型/芯片: {board}: {selected}")
    entries = json.loads((build_dir / "compile_commands.json").read_text())
    board_dir = str((_BOARDS_DIR / board).resolve()) + os.sep
    if not any(str(Path(entry["file"]).resolve()).startswith(board_dir) for entry in entries):
        raise RuntimeError(f"实际编译未包含板级源码: {board}")


def zip_bin(name: str, version: str, build_dir: Path = ROOT / "build") -> None:
    output = ROOT / "releases" / f"v{version}_{name}.zip"
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(build_dir / "merged-binary.bin", arcname="merged-binary.bin")


def build_board(board: str, config_filename: str = "config.json", *,
                filter_name: str | None = None, build_dir: Path = ROOT / "build",
                sdkconfig: Path | None = None) -> None:
    board = resolve_board(board, config_filename)
    config = _load_board(board, config_filename)
    variants = [item for item in config["builds"]
                if not filter_name or item["name"] == filter_name or
                filter_name in item.get("legacy_names", [])]
    if not variants:
        raise ValueError(f"板型 {board} 未定义变体 {filter_name}")
    build_dir = build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.pop("IDF_TARGET", None)
    for variant in variants:
        symbol = _board_symbol(board, config, variant)
        options = variant.get("sdkconfig_append", [])
        if variant.get("build_options"):
            raise ValueError(f"变体 {variant['name']} 的 build_options 尚未映射到本地接口")
        cache = build_dir / "CMakeCache.txt"
        if cache.exists():
            text = cache.read_text()
            previous = re.search(r"^IDF_TARGET[^=]*=(.+)$", text, re.M)
            if previous and previous[1] != config["target"]:
                subprocess.run(_idf_command() + ["-B", str(build_dir), "fullclean"],
                               cwd=ROOT, env=environment, check=True)
                build_dir.mkdir(parents=True, exist_ok=True)
        config_file = build_dir / "sdkconfig"
        if sdkconfig:
            source = sdkconfig.resolve()
            if source != config_file:
                shutil.copyfile(source, config_file)
        else:
            # Supply the complete selection before the first Kconfig pass.
            # Reusing a generated sdkconfig would retain another board's
            # Flash, PSRAM and partition choices because defaults cannot
            # override previously saved values.
            config_file.write_text("\n".join([f"{symbol}=y", *options]) + "\n")
        command = _idf_command() + [
            "-B", str(build_dir), f"-DSDKCONFIG={config_file}",
            f"-DIDF_TARGET={config['target']}", f"-DBOARD_DIR={board}",
            f"-DBOARD_NAME={variant['name']}", "reconfigure",
        ]
        subprocess.run(command, cwd=ROOT, env=environment, check=True)
        _validate_selection(board, variant, build_dir, config_filename)
        subprocess.run(_idf_command() + ["-B", str(build_dir), "build"],
                       cwd=ROOT, env=environment, check=True)
        _validate_selection(board, variant, build_dir, config_filename)
        zip_bin(variant["name"], get_project_version(), build_dir)


release = build_board


def select_changed(variants: list[dict], paths: list[str]) -> list[dict]:
    affected = set()
    for path in paths:
        if path.startswith(("docs/", ".agents/", "tests/")) or path.endswith(".md"):
            continue
        if path.startswith("main/boards/") and not path.startswith(
                ("main/boards/common/", "main/boards/common_esp32s3/")):
            boards = {item["board"] for item in variants
                      if path.startswith("main/boards/" + item["board"] + "/")}
            if not boards:
                return variants
            affected.update(boards)
        else:
            return variants
    return [item for item in variants if item["board"] in affected]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", nargs="?", help="板型目录、原板型身份或 all")
    parser.add_argument("-c", "--config", default="config.json")
    parser.add_argument("--name", help="发布变体名称")
    parser.add_argument("--list-boards", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--select-changed", action="store_true", help="从标准输入读取变更路径")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--sdkconfig", type=Path, help="使用已有 menuconfig 配置")
    args = parser.parse_args()
    try:
        variants = _collect_variants(args.config)
        if args.select_changed:
            print(json.dumps(select_changed(variants, sys.stdin.read().splitlines())))
            return 0
        if args.list_boards:
            if args.json:
                print(json.dumps(variants))
            else:
                for item in variants:
                    print(f"{item['board']}: {item['name']} ({item['target']})")
            return 0
        if not args.board:
            parser.error("需要板型目录，或使用 --list-boards")
        if args.board == "all" and (args.name or args.sdkconfig):
            parser.error("all 不支持 --name 或 --sdkconfig")
        boards = sorted({item["board"] for item in variants}) if args.board == "all" else [args.board]
        for board in boards:
            build_board(board, args.config, filter_name=args.name,
                        build_dir=args.build_dir, sdkconfig=args.sdkconfig)
        return 0
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

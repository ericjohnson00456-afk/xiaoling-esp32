#!/usr/bin/env python3
"""通过串口安装或导出小聆小应用：miniapp.py push app.lua / pull app.lua。"""

import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import secrets
import sys
import tempfile
import time


class TransferError(Exception):
    pass


class Client:
    def __init__(self, port, timeout=30):
        self.port = port
        self.timeout = timeout
        self.token = secrets.token_hex(8)
        self.pending = bytearray()
        self.marker = f"@miniapp {self.token} ".encode()

    def request(self, verb, *arguments):
        command = " ".join(("miniapp", self.token, verb, *map(str, arguments))) + "\n"
        if len(command.encode()) >= 256:
            raise TransferError("传输命令超出固件行长度限制")
        self.port.write(command.encode())
        started = time.monotonic()
        deadline = started + self.timeout
        retry_at = started + 1
        while time.monotonic() < deadline:
            # A prior terminal may have left the board in reset. Only the
            # read-only handshake may be repeated while its console starts.
            if verb == "hello" and time.monotonic() >= retry_at:
                self.port.write(command.encode())
                retry_at = time.monotonic() + 1
            self.pending.extend(self.port.read(min(getattr(self.port, "in_waiting", 0), 4096) or 1))
            while b"\n" in self.pending:
                line, _, rest = self.pending.partition(b"\n")
                self.pending = bytearray(rest)
                start = line.find(self.marker)
                if start < 0:
                    continue
                fields = line[start + len(self.marker):].strip().decode("ascii", "replace").split()
                if fields and fields[0] == "error":
                    raise TransferError("设备拒绝传输：" + " ".join(fields[1:]))
                if fields and fields[0] == verb:
                    return fields[1:]
            # Logs are mixed into the console. Never keep or print unbounded logs.
            if len(self.pending) > 8192:
                self.pending = self.pending[-4096:]
        raise TransferError(f"等待设备 {verb} 响应超时")

    def hello(self):
        fields = self.request("hello")
        if len(fields) != 3 or fields[0] != "1":
            raise TransferError("不支持的侧载协议，请更新设备固件")
        try:
            limit, chunk = map(int, fields[1:])
        except ValueError as error:
            raise TransferError("设备返回了无效的传输限制") from error
        if not 1 <= limit <= 262144 or not 1 <= chunk <= 120:
            raise TransferError("设备返回的传输限制超出范围")
        return limit, chunk

    def abort(self):
        try:
            self.port.write(f"miniapp {self.token} abort\n".encode())
        except (OSError, TimeoutError):
            pass

    def push(self, source, app_id):
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,121}", app_id):
            raise TransferError("应用 ID 需为 1–121 个英文字母、数字、下划线或连字符；可用 --id 指定")
        limit, chunk_size = self.hello()
        # Keep the entire command below the 128-byte UART RX FIFO capacity,
        # including base64, the token and the largest source offset.
        chunk_size = min(chunk_size, 60)
        if not source or len(source) > limit or b"\0" in source:
            raise TransferError(f"源码必须非空、不含 NUL，且不超过 {limit} 字节")
        digest = hashlib.md5(source).hexdigest()
        try:
            if self.request("push", app_id, len(source), digest) != ["0"]:
                raise TransferError("设备未接受安装请求")
            for offset in range(0, len(source), chunk_size):
                chunk = source[offset:offset + chunk_size]
                encoded = base64.b64encode(chunk).decode("ascii")
                if self.request("put", offset, encoded) != [str(offset + len(chunk))]:
                    raise TransferError("设备确认的传输位置不匹配")
            if self.request("commit") != [digest]:
                raise TransferError("设备未确认安装完成")
        finally:
            self.abort()
        return digest

    def pull(self):
        limit, chunk_size = self.hello()
        try:
            fields = self.request("pull")
            if len(fields) != 2 or not re.fullmatch(r"[0-9a-f]{32}", fields[1]):
                raise TransferError("设备返回了无效的源码信息")
            try:
                size = int(fields[0])
            except ValueError as error:
                raise TransferError("设备返回了无效的源码长度") from error
            if not 1 <= size <= limit:
                raise TransferError("设备返回的源码长度超出范围")
            source = bytearray()
            while len(source) < size:
                fields_chunk = self.request("get", len(source))
                if len(fields_chunk) != 2 or fields_chunk[0] != str(len(source)):
                    raise TransferError("设备返回的传输位置不匹配")
                try:
                    chunk = base64.b64decode(fields_chunk[1], validate=True)
                except ValueError as error:
                    raise TransferError("设备返回了无效的源码数据") from error
                if len(chunk) != min(chunk_size, size - len(source)):
                    raise TransferError("设备返回的源码分块长度不匹配")
                source.extend(chunk)
            if hashlib.md5(source).hexdigest() != fields[1]:
                raise TransferError("源码 MD5 校验失败，未写入文件")
            return bytes(source)
        finally:
            self.abort()


def save_file(path, source):
    """Keep an existing destination intact if transfer or writing fails."""
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as file:
            temporary = Path(file.name)
            file.write(source)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def open_port(device):
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as error:
        raise TransferError("缺少 pyserial；请先运行 python3 -m pip install pyserial") from error
    if not device:
        candidates = [p.device for p in list_ports.comports()
                      if p.vid in (0x303A, 0x10C4, 0x1A86, 0x0403)]
        if len(candidates) != 1:
            raise TransferError("无法确定唯一 ESP32 串口，请用 --port 指定")
        device = candidates[0]

    class ConsolePort(serial.Serial):
        # Opening the S3 USB Serial/JTAG port must not toggle DTR/RTS and reboot it.
        def _update_dtr_state(self):
            pass

        def _update_rts_state(self):
            pass

    options = {"exclusive": True} if os.name == "posix" else {}
    if os.name == "nt":
        # Windows configures control lines directly in SetCommState, bypassing
        # the hooks above. Set the cached idle states before opening the handle;
        # enabled lines can leave the board in reset when that handle closes.
        port = ConsolePort(None, baudrate=115200, timeout=0.1, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = device
        port.open()
        return port
    return ConsolePort(device, baudrate=115200, timeout=0.1, write_timeout=2, **options)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="设备串口；仅连接一台 ESP32 时自动选择")
    commands = parser.add_subparsers(dest="command", required=True)
    push = commands.add_parser("push", help="安装并运行 Lua 文件")
    push.add_argument("file", type=Path)
    push.add_argument("--id", help="应用 ID，默认使用不含扩展名的文件名")
    pull = commands.add_parser("pull", help="导出最近一次成功安装的源码")
    pull.add_argument("file", type=Path, nargs="?", default=Path("miniapp.lua"))
    args = parser.parse_args(argv)
    try:
        source = args.file.read_bytes() if args.command == "push" else None
        with open_port(args.port) as port:
            client = Client(port)
            if args.command == "push":
                client.push(source, args.id or args.file.stem)
                print(f"已安装并运行 {args.file.name}（{len(source)} 字节）")
            else:
                source = client.pull()
                save_file(args.file, source)
                print(f"已导出到 {args.file}（{len(source)} 字节）")
    except KeyboardInterrupt:
        print("传输已取消", file=sys.stderr)
        return 130
    except (TransferError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

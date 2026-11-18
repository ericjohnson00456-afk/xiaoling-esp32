import base64
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("miniapp", Path(__file__).parents[2] / "scripts/miniapp.py")
miniapp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(miniapp)


class Console:
    def __init__(self, source=b"", corrupt=False):
        self.source = source
        self.buffer = bytearray()
        self.commands = []
        self.corrupt = corrupt
        self.installed = False

    @property
    def in_waiting(self):
        return min(7, len(self.buffer))  # Fragment both markers and base64 data.

    def write(self, data):
        self.commands.append(data)
        words = data.decode().split()
        _, token, verb, *args = words
        reply = verb
        if verb == "hello":
            reply += " 1 65536 120"
        elif verb == "push":
            self.source = b""
            self.expected_hash = args[2]
            reply += " 0"
        elif verb == "put":
            assert int(args[0]) == len(self.source)
            self.source += base64.b64decode(args[1])
            reply += f" {len(self.source) + int(self.corrupt)}"
        elif verb == "commit":
            assert hashlib.md5(self.source).hexdigest() == self.expected_hash
            self.installed = True
            reply += " " + self.expected_hash
        elif verb == "pull":
            reply += f" {len(self.source)} {hashlib.md5(self.source).hexdigest()}"
        elif verb == "get":
            offset = int(args[0])
            chunk = self.source[offset:offset + 120]
            if self.corrupt:
                chunk = bytes([chunk[0] ^ 1]) + chunk[1:]
            reply += f" {offset} {base64.b64encode(chunk).decode()}"
        self.buffer.extend(b"I (1) unrelated console output\n@miniapp 0000000000000000 hello 9\n")
        self.buffer.extend(f"xiaoling> @miniapp {token} {reply}\r\n".encode())

    def read(self, size):
        chunk = self.buffer[:min(size, 7)]
        del self.buffer[:len(chunk)]
        return chunk


class SerialTests(unittest.TestCase):
    def test_push_chunks_and_repeated_install(self):
        source = ("-- 中文注释\nfunction on_tick() end\n" * 32).encode()
        port = Console()
        client = miniapp.Client(port)
        client.push(source, "counter")
        self.assertTrue(port.installed)
        self.assertEqual(port.source, source)
        client.push(source, "counter")
        self.assertEqual(sum(b" commit\n" in c for c in port.commands), 2)
        self.assertTrue(all(len(c) < 256 for c in port.commands))

    def test_pull_snapshot_and_atomic_destination(self):
        source = b"function on_tick() end\n" * 32
        result = miniapp.Client(Console(source)).pull()
        self.assertEqual(result, source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.lua"
            path.write_bytes(b"old source")
            miniapp.save_file(path, result)
            self.assertEqual(path.read_bytes(), source)
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def test_corrupt_pull_is_rejected_and_aborted(self):
        port = Console(b"source" * 40, corrupt=True)
        with self.assertRaisesRegex(miniapp.TransferError, "MD5"):
            miniapp.Client(port).pull()
        self.assertIn(b" abort\n", port.commands[-1])

    def test_wrong_ack_does_not_commit(self):
        port = Console(corrupt=True)
        with self.assertRaises(miniapp.TransferError):
            miniapp.Client(port).push(b"source", "app")
        self.assertFalse(port.installed)
        self.assertIn(b" abort\n", port.commands[-1])

    def test_invalid_source_and_id(self):
        for source, app_id in [(b"", "ok"), (b"a\0b", "ok"), (b"a", "../bad")]:
            port = Console()
            with self.assertRaises(miniapp.TransferError):
                miniapp.Client(port).push(source, app_id)
            self.assertFalse(port.installed)

    def test_timeout_aborts_without_commit(self):
        port = Console()
        original_write = port.write
        def write(data):
            original_write(data)
            if b" put " in data:
                port.buffer.clear()
        port.write = write
        with self.assertRaisesRegex(miniapp.TransferError, "超时"):
            miniapp.Client(port, timeout=0.01).push(b"source", "app")
        self.assertFalse(port.installed)
        self.assertIn(b" abort\n", port.commands[-1])


if __name__ == "__main__":
    unittest.main()

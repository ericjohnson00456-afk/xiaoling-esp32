import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from romfs import TYPE_FILE, parse_header, parse_superblock, traverse_directory


def files_in_image(path):
    data = path.read_bytes()
    size, _, root = parse_superblock(data)
    node = parse_header(data, root, size)
    entries = []
    traverse_directory(data, node["spec_info"], size, "/", entries, set())
    return {
        name: data[info["data_offset"]:info["data_offset"] + info["size"]]
        for name, info in entries if info["node_type"] == TYPE_FILE
    }


class TextFontPackagingTest(unittest.TestCase):
    def test_unresolved_lfs_resources_fail_packaging(self):
        for relative, partition in (
            ("respak/font/font_puhui_common_16_4.bin", "respak"),
            ("tone/000_greeting.mp3", "tone"),
            ("emoji/happy/001.png", "emoji"),
        ):
            with self.subTest(resource=relative), tempfile.TemporaryDirectory() as temp:
                base = Path(temp)
                source = base / "resources"
                for locale in ("zh-CN", "en-US"):
                    (source / "respak/locales" / locale).mkdir(parents=True)
                asset = source / relative
                asset.parent.mkdir(parents=True, exist_ok=True)
                asset.write_text(
                    "version https://git-lfs.github.com/spec/v1\n"
                    "oid sha256:" + "0" * 64 + "\nsize 123456\n"
                )
                output = base / "output"
                result = subprocess.run([
                    sys.executable, str(ROOT / "scripts/build_split_resources.py"),
                    "--resource-root", str(source), "--output-dir", str(output),
                ], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Unresolved Git LFS pointer", result.stderr)
                self.assertIn(asset.name, result.stderr)
                self.assertIn("git lfs pull", result.stderr)
                self.assertFalse((output / (partition + ".bin")).exists())

    def test_small_board_font_replaces_text_only_and_preserves_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source = base / "resources"
            original = source / "respak/font/font_puhui_common_20_4.bin"
            icon = source / "respak/font/font_awesome_20_4.bin"
            original.parent.mkdir(parents=True)
            original.write_bytes(b"large-text-font" * 4096)
            icon.write_bytes(b"icon-font")
            for locale in ("zh-CN", "en-US"):
                directory = source / "respak/locales" / locale
                directory.mkdir(parents=True)
                (directory / "language.json").write_text("{}")
            small = base / "font_puhui_common_16_4.bin"
            small.write_bytes(b"small-text-font")
            budget = base / "font_puhui_common_20_4.bin"
            budget.write_bytes(b"budget-text-font")
            for name, extra in (("default", []),
                                ("small", ["--text-font-file", str(small)]),
                                ("small_with_budget", ["--font-file", str(budget),
                                                       "--text-font-file", str(small)])):
                output = base / name
                subprocess.run([
                    sys.executable, str(ROOT / "scripts/build_split_resources.py"),
                    "--resource-root", str(source), "--output-dir", str(output), *extra,
                ], check=True, capture_output=True)
                files = files_in_image(output / "respak.bin")
                self.assertEqual(files["/font/font_awesome_20_4.bin"], b"icon-font")
                selected = small.name if extra else original.name
                self.assertEqual(files["/font/" + selected],
                                 small.read_bytes() if extra else original.read_bytes())
                absent = original.name if extra else small.name
                self.assertNotIn("/font/" + absent, files)
                self.assertEqual(parse_superblock((output / "wake_word.bin").read_bytes())[1],
                                 "wake_word")
            self.assertGreater((base / "default/respak.bin").stat().st_size,
                               (base / "small/respak.bin").stat().st_size)
            self.assertTrue(original.is_file())
            self.assertEqual(icon.read_bytes(), b"icon-font")


if __name__ == "__main__":
    unittest.main()

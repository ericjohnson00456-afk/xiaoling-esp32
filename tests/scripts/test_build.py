import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("board_build", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name)
        self.boards = self.root / "main/boards"
        self.boards.mkdir(parents=True)
        (self.root / "CMakeLists.txt").write_text('set(PROJECT_VER "2.1.1")\n')
        self.patches = [patch.object(build, "ROOT", self.root),
                        patch.object(build, "_BOARDS_DIR", self.boards)]
        for item in self.patches:
            item.start()
            self.addCleanup(item.stop)
        (self.root / "main/CMakeLists.txt").write_text('''
if(CONFIG_BOARD_TYPE_CORE)
    set(BOARD_DIR "m5stack/core-s3")
elseif(CONFIG_BOARD_TYPE_BOX)
    set(BOARD_DIR "alientek/box")
endif()
''')
        (self.root / "main/Kconfig.projbuild").write_text('''
choice BOARD_TYPE
    config BOARD_TYPE_CORE
        bool "CoreS3"
        depends on IDF_TARGET_ESP32S3
    config BOARD_TYPE_BOX
        bool "BOX"
        depends on IDF_TARGET_ESP32S3
endchoice
''')
        for board, identity in [("m5stack/core-s3", "m5stack-core-s3"),
                                ("alientek/box", "atk-dnesp32s3-box")]:
            directory = self.boards / board
            directory.mkdir(parents=True)
            (directory / "board.cc").write_text("// board fixture\n")
            (directory / "config.json").write_text(json.dumps({
                "manufacturer": board.split("/")[0], "type": identity,
                "target": "esp32s3", "builds": [{"name": identity}],
            }))

    def test_nested_directories_preserve_reported_identity_and_variant_names(self):
        variants = build._collect_variants()
        self.assertEqual({item["board"] for item in variants},
                         {"m5stack/core-s3", "alientek/box"})
        self.assertEqual(build.resolve_board("m5stack-core-s3"), "m5stack/core-s3")
        self.assertEqual(build.resolve_board("atk-dnesp32s3-box"), "alientek/box")

    def test_mismatched_chip_is_rejected_before_any_build(self):
        path = self.boards / "m5stack/core-s3/config.json"
        config = json.loads(path.read_text())
        config["target"] = "esp32"
        path.write_text(json.dumps(config))
        with patch.object(build.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "不支持芯片"):
                build.build_board("m5stack/core-s3", build_dir=self.root / "build")
            run.assert_not_called()

    def test_explicit_symbol_for_another_directory_is_rejected(self):
        config = build._load_board("m5stack/core-s3")
        variant = {"name": "m5stack-core-s3", "board_type_config": "CONFIG_BOARD_TYPE_BOX"}
        with self.assertRaises(ValueError):
            build._board_symbol("m5stack/core-s3", config, variant)

    def test_manufacturer_mismatch_and_path_traversal_are_rejected(self):
        with self.assertRaises(ValueError):
            build.resolve_board("../outside")
        path = self.boards / "m5stack/core-s3/config.json"
        config = json.loads(path.read_text())
        config["manufacturer"] = "alientek"
        path.write_text(json.dumps(config))
        with self.assertRaises(ValueError):
            build._collect_variants()

    def test_new_variant_does_not_keep_another_boards_flash_and_psram(self):
        directory = self.root / "build"
        directory.mkdir()
        (directory / "sdkconfig").write_text(
            'CONFIG_BOARD_TYPE_BOX=y\nCONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\n')

        def run(command, **kwargs):
            if command[-1] == "build":
                return
            self.assertIn("-DBOARD_DIR=m5stack/core-s3", command)
            selection = (directory / "sdkconfig").read_text()
            self.assertIn("CONFIG_BOARD_TYPE_CORE=y", selection)
            self.assertNotIn("CONFIG_BOARD_TYPE_BOX", selection)
            self.assertNotIn("CONFIG_ESPTOOLPY_FLASHSIZE_4MB", selection)
            with (directory / "sdkconfig").open("a") as output:
                output.write('CONFIG_IDF_TARGET="esp32s3"\n')
            (directory / "compile_commands.json").write_text(json.dumps([
                {"file": str(self.boards / "m5stack/core-s3/board.cc")},
            ]))
            (directory / "merged-binary.bin").write_bytes(b"current firmware")

        with patch.dict(build.os.environ, {"IDF_PATH": "/sdk"}), \
             patch.object(build.subprocess, "run", side_effect=run):
            build.build_board("m5stack-core-s3", build_dir=directory)
        with zipfile.ZipFile(self.root / "releases/v2.1.1_m5stack-core-s3.zip") as archive:
            self.assertEqual(archive.read("merged-binary.bin"), b"current firmware")

    def test_stale_release_archive_is_replaced(self):
        directory = self.root / "build"
        directory.mkdir()
        (directory / "merged-binary.bin").write_bytes(b"new firmware")
        release = self.root / "releases/v2.1.1_m5stack-core-s3.zip"
        release.parent.mkdir()
        release.write_bytes(b"stale archive")
        build.zip_bin("m5stack-core-s3", "2.1.1", directory)
        with zipfile.ZipFile(release) as archive:
            self.assertEqual(archive.read("merged-binary.bin"), b"new firmware")

    def test_changed_board_selection_uses_full_nested_path(self):
        variants = build._collect_variants()
        result = build.select_changed(variants, ["main/boards/alientek/box/board.cc"])
        self.assertEqual([item["board"] for item in result], ["alientek/box"])
        self.assertEqual(build.select_changed(variants, ["docs/custom-board.md"]), [])
        self.assertEqual(build.select_changed(variants, ["main/application.cc"]), variants)


if __name__ == "__main__":
    unittest.main()

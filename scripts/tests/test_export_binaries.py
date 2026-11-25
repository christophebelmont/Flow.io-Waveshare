import runpy
import json
import hashlib
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class _DummyEnv:
    def AddPostAction(self, *args, **kwargs):
        pass

    def Depends(self, *args, **kwargs):
        pass

    def AlwaysBuild(self, *args, **kwargs):
        pass

    def GetProjectOption(self, name):
        if name == "board_build.partitions":
            return str(ROOT / "partition_waveshare_ota_16mb.csv")
        raise KeyError(name)

    def subst(self, value):
        return "Flowio-waveshare-esp32-s3" if value == "$PIOENV" else value

    def DataToBin(self, *args, **kwargs):
        return "spiffs.bin"


SCRIPT = Path(__file__).resolve().parents[1] / "export_binaries.py"
MODULE = runpy.run_path(
    str(SCRIPT),
    init_globals={"Import": lambda name: None, "env": _DummyEnv()},
)


class NextionArtifactFilenameTests(unittest.TestCase):
    def test_release_filenames_expose_exact_compatibility(self):
        expected = {
            "FlowIO_Nextion_NX4832K035_011-6.0.0.tft": "NX4832K035_011",
            "FlowIO_Nextion_NX8048P050_011-6.0.0.tft": "NX8048P050_011",
            "FlowIO_Nextion_NX8048P070_011-6.0.0.tft": "NX8048P070_011",
        }
        for filename, compatibility in expected.items():
            with self.subTest(filename=filename):
                parsed = MODULE["_parse_nextion_filename"](filename)
                self.assertEqual(compatibility, parsed["display_compatibility"])
                self.assertEqual("6.0.0", parsed["version"])

    def test_touch_specific_artifact_name_is_rejected(self):
        with self.assertRaises(ValueError):
            MODULE["_parse_nextion_filename"](
                "FlowIO_Nextion_NX8048P050_011C-6.0.0.tft"
            )

    def test_non_canonical_filename_is_rejected(self):
        with self.assertRaises(ValueError):
            MODULE["_parse_nextion_filename"](
                "Flowio_Intelligent_800x480-6.0.0.tft"
            )


class ReleasePackageTests(unittest.TestCase):
    def test_diagnostic_exports_are_isolated(self):
        function = MODULE["_binary_dir"]
        globals_ = function.__globals__
        original_env, original_project = globals_["env"], globals_["_project_dir"]
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            try:
                globals_["_project_dir"] = lambda: root
                globals_["env"] = {"FLOW_MEMORY_DIAGNOSTICS": True}
                self.assertEqual(function(), root / "binary" / "diagnostics")
                globals_["env"] = {"FLOW_MEMORY_DIAGNOSTICS": False}
                self.assertEqual(function(), root / "binary")
            finally:
                globals_["env"], globals_["_project_dir"] = original_env, original_project

    def test_filesystem_size_comes_from_partition_table(self):
        self.assertEqual(0x180000, MODULE["_expected_filesystem_size"]())

    def test_package_contains_streamable_complete_release(self):
        expected_filesystem_size = MODULE["_expected_filesystem_size"]()
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            firmware = output / "flowios3-2.0.1.bin"
            filesystem = output / "flowios3-spiffs-2.0.1.bin"
            firmware.write_bytes(b"firmware-image")
            filesystem.write_bytes(b"\xff" * expected_filesystem_size)

            function = MODULE["_write_release_package"]
            original_binary_dir = function.__globals__["_binary_dir"]
            original_project_dir = function.__globals__["_project_dir"]
            function.__globals__["_binary_dir"] = lambda: output
            function.__globals__["_project_dir"] = lambda: output
            try:
                function("2.0.1")
            finally:
                function.__globals__["_binary_dir"] = original_binary_dir
                function.__globals__["_project_dir"] = original_project_dir

            with zipfile.ZipFile(output / "flowio-2.0.1.zip") as archive:
                self.assertEqual(
                    ["manifest.json", "firmware.bin", "spiffs.bin"],
                    archive.namelist(),
                )
                self.assertTrue(
                    all(item.compress_type == zipfile.ZIP_STORED for item in archive.infolist())
                )
                manifest = json.loads(archive.read("manifest.json"))
                self.assertEqual("WaveshareESP32S3", manifest["hardware"])
                self.assertEqual(len(firmware.read_bytes()), manifest["firmware"]["size"])
                self.assertEqual(expected_filesystem_size, manifest["filesystem"]["size"])

            # Remote OTA needs these *two additional files* in binary/, not
            # just the browser ZIP. Each must refer to the flat binary names.
            expected_sha_fw = hashlib.sha256(firmware.read_bytes()).hexdigest()
            expected_sha_fs = hashlib.sha256(filesystem.read_bytes()).hexdigest()
            for name in ("flowio-release-2.0.1.json", "release.json"):
                path = output / name
                self.assertTrue(path.is_file(), f"remote OTA manifest missing: {name}")
                remote = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual("flowios3-2.0.1.bin", remote["firmware"]["file"])
                self.assertEqual("flowios3-spiffs-2.0.1.bin", remote["filesystem"]["file"])
                self.assertEqual(expected_sha_fw, remote["firmware"]["sha256"])
                self.assertEqual(expected_sha_fs, remote["filesystem"]["sha256"])
                self.assertEqual(firmware.stat().st_size, remote["firmware"]["size"])
                self.assertEqual(filesystem.stat().st_size, remote["filesystem"]["size"])

    def test_missing_remote_manifests_are_recreated_even_with_existing_zip(self):
        function = MODULE["_write_release_package"]
        globals_ = function.__globals__
        original_binary_dir = globals_["_binary_dir"]
        original_project_dir = globals_["_project_dir"]
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            (output / "flowios3-2.0.1.bin").write_bytes(b"firmware-image")
            (output / "flowios3-spiffs-2.0.1.bin").write_bytes(b"\xff" * 0x180000)
            try:
                globals_["_binary_dir"] = lambda: output
                globals_["_project_dir"] = lambda: output
                function("2.0.1")
                (output / "flowio-release-2.0.1.json").unlink()
                (output / "release.json").unlink()
                self.assertTrue((output / "flowio-2.0.1.zip").is_file())

                function("2.0.1")  # A second build repairs both missing files.
                self.assertTrue((output / "flowio-release-2.0.1.json").is_file())
                self.assertTrue((output / "release.json").is_file())
            finally:
                globals_["_binary_dir"] = original_binary_dir
                globals_["_project_dir"] = original_project_dir

    def test_release_zip_is_not_published_before_remote_manifests(self):
        function = MODULE["_write_release_package"]
        globals_ = function.__globals__
        original_binary_dir = globals_["_binary_dir"]
        original_project_dir = globals_["_project_dir"]
        original_zipfile = globals_["zipfile"]
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            (output / "flowios3-2.0.1.bin").write_bytes(b"firmware-image")
            (output / "flowios3-spiffs-2.0.1.bin").write_bytes(b"\xff" * 0x180000)

            # Fail right when ZIP writing starts. Both remote OTA manifests
            # must already be published. No partial ZIP may be left behind.
            class FailingZip:
                def ZipFile(self, *args, **kwargs):
                    self_file = output / "flowio-release-2.0.1.json"
                    current = output / "release.json"
                    assert self_file.is_file() and current.is_file()
                    raise OSError("simulated ZIP writer failure")

                ZIP_STORED = zipfile.ZIP_STORED

            try:
                globals_["_binary_dir"] = lambda: output
                globals_["_project_dir"] = lambda: output
                globals_["zipfile"] = FailingZip()
                with self.assertRaises(OSError):
                    function("2.0.1")
                self.assertFalse((output / "flowio-2.0.1.zip").exists())
                self.assertFalse((output / "flowio-2.0.1.zip.tmp").exists())
            finally:
                globals_["_binary_dir"] = original_binary_dir
                globals_["_project_dir"] = original_project_dir
                globals_["zipfile"] = original_zipfile

    def test_firmware_build_fails_without_both_remote_manifests(self):
        verify = MODULE["_assert_complete_release"]
        globals_ = verify.__globals__
        original_binary_dir = globals_["_binary_dir"]
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            for name in (
                "flowios3-2.0.1.bin",
                "flowios3-spiffs-2.0.1.bin",
                "flowio-2.0.1.zip",
            ):
                (output / name).write_bytes(b"test")
            globals_["_binary_dir"] = lambda: output
            try:
                with self.assertRaisesRegex(RuntimeError, "INCOMPLETE OTA RELEASE"):
                    verify("2.0.1")

                manifest = {
                    "version": "2.0.1",
                    "firmware": {"file": "flowios3-2.0.1.bin"},
                    "filesystem": {"file": "flowios3-spiffs-2.0.1.bin"},
                }
                content = json.dumps(manifest) + "\n"
                (output / "flowio-release-2.0.1.json").write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "INCOMPLETE OTA RELEASE"):
                    verify("2.0.1")
                (output / "release.json").write_text(content, encoding="utf-8")
                verify("2.0.1")
                manifest["filesystem"]["file"] = "wrong-spiffs.bin"
                (output / "release.json").write_text(json.dumps(manifest), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "invalid remote OTA release manifest"):
                    verify("2.0.1")
            finally:
                globals_["_binary_dir"] = original_binary_dir

    def test_wrong_filesystem_size_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            (output / "flowios3-2.0.1.bin").write_bytes(b"firmware-image")
            (output / "flowios3-spiffs-2.0.1.bin").write_bytes(b"\xff" * 16)

            function = MODULE["_write_release_package"]
            original_binary_dir = function.__globals__["_binary_dir"]
            original_project_dir = function.__globals__["_project_dir"]
            function.__globals__["_binary_dir"] = lambda: output
            function.__globals__["_project_dir"] = lambda: output
            try:
                with self.assertRaises(RuntimeError):
                    function("2.0.1")
            finally:
                function.__globals__["_binary_dir"] = original_binary_dir
                function.__globals__["_project_dir"] = original_project_dir

            self.assertFalse((output / "flowio-2.0.1.zip").exists())

if __name__ == "__main__":
    unittest.main()

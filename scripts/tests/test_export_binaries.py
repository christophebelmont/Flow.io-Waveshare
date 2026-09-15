import runpy
import json
import tempfile
import unittest
import zipfile
from pathlib import Path


class _DummyEnv:
    def AddPostAction(self, *args, **kwargs):
        pass

    def Depends(self, *args, **kwargs):
        pass

    def AlwaysBuild(self, *args, **kwargs):
        pass

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
    def test_package_contains_streamable_complete_release(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            firmware = output / "flowios3-2.0.1.bin"
            filesystem = output / "flowios3-spiffs-2.0.1.bin"
            firmware.write_bytes(b"firmware-image")
            filesystem.write_bytes(b"\xff" * 0x180000)

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
                self.assertEqual(0x180000, manifest["filesystem"]["size"])

if __name__ == "__main__":
    unittest.main()

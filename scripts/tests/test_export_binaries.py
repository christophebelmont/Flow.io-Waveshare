import runpy
import unittest
from pathlib import Path


class _DummyEnv:
    def AddPostAction(self, *args, **kwargs):
        pass


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


if __name__ == "__main__":
    unittest.main()

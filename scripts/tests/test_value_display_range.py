"""Host checks for display-range validation and stable-ID defaults."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ValueDisplayRangeTests(unittest.TestCase):
    def test_validation_and_defaults(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "value_display_range"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-Isrc", "-Iinclude", "test/host/value_display_range.cpp", "-o", str(executable)],
                cwd=ROOT, check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()

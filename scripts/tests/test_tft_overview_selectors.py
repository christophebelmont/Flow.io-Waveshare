"""Compile actual TFT row drawing/state reads against a bounded display recorder."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class TftOverviewSelectorsTests(unittest.TestCase):
    def test_native_resolution_and_configured_choices(self):
        module = ROOT / "src/Modules/TFTModuleS3"
        header = (module / "TFTModuleS3.h").read_text()
        source = (module / "TFTModuleS3.cpp").read_text()
        gfx = ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/Adafruit GFX Library"
        fixture = (ROOT / "test/host/tft_overview_selectors.cpp").read_text()
        fixture = fixture.replace("// FONT_TYPES", (gfx / "gfxfont.h").read_text())
        font = (gfx / "Fonts/FreeSans9pt7b.h").read_text()
        fixture = fixture.replace("// FONT_DATA", "\n".join(line for line in font.splitlines() if not line.startswith("#")))
        constants = function(source, "constexpr uint16_t rgb565_") + "\n"
        for name in ("kHeaderH", "kSidePad", "kOverviewSelectorRows", "kOverviewSelectorGap",
                     "kColorSelected", "kColorBg", "kColorCardBg", "kColorCardBorder", "kColorMuted"):
            constants += re.search(r"constexpr [^\n]+ " + name + r" = [^\n]+;", source).group(0) + "\n"
        fixture = fixture.replace("// PRODUCTION_CONSTANTS", constants)
        geometry = function(source, "struct Rect") + ";\n"
        geometry += function(source, "Rect overviewTopCardRect_") + "\n"
        geometry += function(source, "Rect overviewSelectorButtonRect_")
        fixture = fixture.replace("// PRODUCTION_GEOMETRY", geometry)
        fixture = fixture.replace("// PRODUCTION_STATE", function(header, "struct OverviewStatusRenderState") + ";")
        fixture = fixture.replace("// PRODUCTION_CHOICE", function(header, "struct OverviewChoice") + ";")
        methods = "\n".join(function(source, signature) for signature in (
            "void TFTModuleS3::drawOverviewSelectorRow_", "void TFTModuleS3::drawOverviewSelectors_",
            "bool TFTModuleS3::overviewStatusEquals_", "void TFTModuleS3::readOverviewStatus_"))
        fixture = fixture.replace("// PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="flow-tft-selectors-") as directory:
            main, binary = Path(directory) / "main.cpp", Path(directory) / "test"
            main.write_text(fixture)
            for invocation in (["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                                "-Isrc", "-Iinclude", str(main), "-o", str(binary)], [str(binary)]):
                result = subprocess.run(invocation, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

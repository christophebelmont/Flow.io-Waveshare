"""Compile actual TFT row drawing/state reads against a bounded display recorder."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class TftOverviewSelectorsTests(unittest.TestCase):
    def test_shared_header_and_icon_regression(self):
        module = ROOT / "src/Modules/TFTModuleS3"
        source = (module / "TFTModuleS3.cpp").read_text()
        header = (module / "TFTModuleS3.h").read_text()
        wave = (module / "FlowIoWaveDarkBitmap.h").read_text()
        render = function(source, "void TFTModuleS3::render_")
        self.assertIn("constexpr int16_t kHeaderH = 32;", source)
        self.assertIn("kFlowIoWaveDarkWidth = 42;", wave)
        self.assertIn("kFlowIoWaveDarkHeight = 20;", wave)
        self.assertIn("drawRGBBitmap(8, 6, kFlowIoWaveDarkBitmap", source)
        self.assertIn("drawHeader_(timeBuf, bars, apMode, (uint8_t)pageCycle", render)
        self.assertIn("const uint8_t selected = activePage % 3U;", source)
        self.assertIn("w - 96, 1, 38", source)  # Larger Wi-Fi dirty rect
        self.assertIn("w - 58, 1, 58", source)  # Clock-only dirty rect
        self.assertIn("if (!layoutDrawn_)", source)  # No header blank on page rotation
        self.assertIn("const bool denseMetrics = true;", render)
        self.assertIn("const bool indicatorsOnly =", render)
        self.assertIn("!fullRedraw && valueOnlyChanged", render)
        self.assertIn("display_.fillRect((int16_t)(x + 26)", source)
        self.assertIn('drawAlarmStateIcon_(stateX, (int16_t)(y + 13), "L"', source)
        self.assertIn('drawAlarmStateIcon_(stateX, (int16_t)(y + h - 13), "C"', source)
        self.assertIn("case 11U: // Low pressure, needle at start", source)
        self.assertIn("case 12U: // High pressure, needle at end", source)
        self.assertIn("case 2U: // Waterfall", source)
        self.assertIn("const GFXfont* valueFont = &FreeSans9pt7b;", source)
        self.assertIn("drawMiniIcon_", header)
        self.assertIn("bool textOnly = false", header)
        self.assertIn("const int16_t unitX =", source)
        self.assertIn("char* space = strchr(labelBuf, ' ');", source)
        self.assertIn("dashboardDarkTint_", source)
        self.assertIn("constexpr int16_t kCardEdge = 1;", source)
        self.assertIn("constexpr int16_t kCardGap = 2;", source)
        self.assertIn("const int16_t labelX = (int16_t)(x + 27);", source)
        self.assertIn("drawGfxText_(&FreeSansBold12pt7b, kColorText", source)
        self.assertIn("rgb565_(246, 78, 82)", source)  # RSSI red->green
        self.assertNotIn("display_.drawFastVLine((int16_t)(x + 32)", source)
        # Keep the separate update progress UI: no global display mode change.
        self.assertIn("void TFTModuleS3::renderUpdate_", source)
        self.assertIn("void TFTModuleS3::drawBootLogo_", source)

    def test_native_resolution_and_configured_choices(self):
        module = ROOT / "src/Modules/TFTModuleS3"
        header = (module / "TFTModuleS3.h").read_text()
        source = (module / "TFTModuleS3.cpp").read_text()
        gfx = ROOT / ".pio/libdeps/Flowio-waveshare-esp32-s3/Adafruit GFX Library"
        fixture = (ROOT / "test/host/tft_overview_selectors.cpp").read_text()
        fixture = fixture.replace("// FONT_TYPES", (gfx / "gfxfont.h").read_text())
        font_files = (
            gfx / "Fonts/FreeSans9pt7b.h",
        )
        font_data = "\n".join(
            line for path in font_files
            for line in path.read_text().splitlines()
            if not line.startswith("#")
        )
        fixture = fixture.replace("// FONT_DATA", font_data)
        constants = function(source, "constexpr uint16_t rgb565_") + "\n"
        for name in ("kHeaderH", "kCardEdge", "kCardGap",
                     "kOverviewSelectorRows", "kOverviewSelectorGap",
                     "kUiCardRadius", "kColorText",
                     "kColorOverviewModeBg", "kColorOverviewPhBg",
                     "kColorOverviewTreatmentBg",
                     "kColorCardBorder", "kColorMuted"):
            constants += re.search(r"constexpr [^\n]+ " + name + r" = [^\n]+;", source).group(0) + "\n"
        fixture = fixture.replace("// PRODUCTION_CONSTANTS", constants)
        geometry = function(source, "struct Rect") + ";\n"
        geometry += function(source, "Rect overviewTopCardRect_") + "\n"
        geometry += function(source, "Rect overviewStatusCardRect_") + "\n"
        geometry += function(source, "Rect metricCardRect_")
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

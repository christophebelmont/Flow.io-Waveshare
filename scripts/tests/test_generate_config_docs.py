import json
import re
import runpy
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "generate_config_docs.py"
MODULE = runpy.run_path(str(SCRIPT), init_globals={"Import": lambda name: None, "env": None})


class ConfigDocsWaveshareSlotTests(unittest.TestCase):
    def test_expression_fields_expand_to_all_slots(self):
        root = SCRIPT.parents[1]
        docs = json.loads((root / "src/Modules/IOModule/text/cfgdocs.fr.json").read_text())["docs"]
        MODULE["_expand_value_slot_docs"](docs, MODULE["VALUE_DERIVED_LAST_SLOT"])
        self.assertEqual(4, MODULE["VALUE_DERIVED_LAST_SLOT"])
        self.assertNotIn("io/value/v05", docs)
        self.assertNotIn("io/value/v15/expr", docs)
        for slot in range(5):
            prefix = f"io/value/v{slot:02d}"
            self.assertEqual("CharArray", docs[prefix + "/expr"]["type"])
            for parameter in range(4):
                self.assertEqual("Double", docs[prefix + f"/k{parameter}"]["type"])
            for removed in ("source", "scale", "offset"):
                self.assertNotIn(prefix + "/" + removed, docs)
        for locale in ("fr", "en"):
            translations = json.loads((root / f"src/Modules/IOModule/text/i18n.{locale}.json").read_text())["translations"]
            MODULE["_expand_value_slot_translations"](translations, MODULE["VALUE_DERIVED_LAST_SLOT"])
            for slot in range(5):
                for field in ("expr", "k0", "k1", "k2", "k3"):
                    self.assertIn(f"cfgdocs.io.value.v{slot:02d}.{field}.help", translations)

    def test_pool_devices_cover_all_sixteen_outputs(self):
        root = SCRIPT.parents[1]
        manifest = json.loads((root / "src/Modules/PoolLogicModule/text/cfgmods.fr.json").read_text())
        result = MODULE["_apply_profile_specific_io_enum_sets"](manifest["meta"], "waveshare")
        entries = result["enum_sets"]["poollogic_device_slot"]
        self.assertEqual(list(range(16)), [item["value"] for item in entries])
        self.assertEqual("pd15 -> d15 [15]", entries[-1]["label"])
        docs = {f"pdm/pd{slot}/enabled": {} for slot in range(17)}
        MODULE["_prune_pool_device_docs"](docs, MODULE["WAVESHARE_DIGITAL_OUTPUT_LAST_SLOT"])
        self.assertEqual(16, len(docs))
        self.assertIn("pdm/pd15/enabled", docs)

    def test_waveshare_analog_configuration_keeps_a20_and_prunes_a21(self):
        docs = {
            "io/input/a15/a15_name": {},
            "io/input/a16/a16_name": {},
            "io/input/a20/a20_name": {},
            "io/input/a21/a21_name": {},
        }

        MODULE["_prune_io_slot_docs"](
            docs,
            analog_last=MODULE["WAVESHARE_ANALOG_LAST_SLOT"],
            digital_last=MODULE["WAVESHARE_DIGITAL_INPUT_LAST_SLOT"],
            output_last=MODULE["WAVESHARE_DIGITAL_OUTPUT_LAST_SLOT"],
        )

        self.assertIn("io/input/a16/a16_name", docs)
        self.assertIn("io/input/a20/a20_name", docs)
        self.assertNotIn("io/input/a21/a21_name", docs)

    def test_tft_derived_runtime_options_match_io_runtime_value_ids(self):
        root = SCRIPT.parents[1]
        io_runtime = (root / "src/Modules/IOModule/IORuntime.h").read_text(encoding="utf-8")
        value_h = (root / "src/Core/Values/Value.h").read_text(encoding="utf-8")
        module_id_h = (root / "src/Core/ModuleId.h").read_text(encoding="utf-8")

        base = int(re.search(r"IO_RUNTIME_UI_DERIVED_BASE\s*=\s*(\d+)", io_runtime).group(1))
        capacity = int(re.search(r"DerivedCapacity\s*=\s*(\d+)", value_h).group(1))
        enum_body = re.search(r"enum class ModuleId\s*:\s*uint8_t\s*\{(?P<body>.*?)\};", module_id_h, re.S).group("body")
        io_index = [
            name.strip().split("=")[0].strip().rstrip(",")
            for name in enum_body.splitlines()
            if name.strip()
        ].index("Io")

        manifest = json.loads((root / "src/Modules/TFTModuleS3/text/cfgmods.fr.json").read_text())
        entries = manifest["meta"]["enum_sets"]["tft_s3_dashboard_runtime_ui"]
        derived = {entry["derived_slot"]: entry["value"] for entry in entries if "derived_slot" in entry}

        self.assertEqual(set(range(capacity)), set(derived))
        for slot, value in derived.items():
            self.assertEqual(io_index * 100 + base + slot, value)

        runtimeui = json.loads((root / "src/Modules/IOModule/text/runtimeui.json").read_text())
        value_ids = {value["valueId"] for value in runtimeui["values"]}
        self.assertTrue(set(range(base, base + capacity)).issubset(value_ids))

    def test_waveshare_analog_configuration_tree_ends_at_a20(self):
        meta = {
            "cfg_tree_aliases": [
                {"display": "io/input/analog/a20", "store": "io/input/a20"},
                {"display": "io/input/analog/a21", "store": "io/input/a21"},
            ],
            "cfg_tree_virtual_branches": [{
                "display": "io/input/analog",
                "children": ["a19", "a20", "a21"],
            }],
        }

        result = MODULE["_prune_io_slot_meta"](
            meta,
            analog_last=MODULE["WAVESHARE_ANALOG_LAST_SLOT"],
            digital_last=MODULE["WAVESHARE_DIGITAL_INPUT_LAST_SLOT"],
            output_last=MODULE["WAVESHARE_DIGITAL_OUTPUT_LAST_SLOT"],
        )

        self.assertEqual(
            ["io/input/analog/a20"],
            [item["display"] for item in result["cfg_tree_aliases"]],
        )
        self.assertEqual(["a19", "a20"], result["cfg_tree_virtual_branches"][0]["children"])


if __name__ == "__main__":
    unittest.main()

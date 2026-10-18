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

    def test_tft_derived_options_use_data_catalog(self):
        from scripts.data_catalog import extend_config
        root = SCRIPT.parents[1]
        meta = json.loads((root / 'src/Modules/TFTModuleS3/text/cfgmods.fr.json').read_text())["meta"]
        extend_config({}, {}, meta)
        entries = meta["enum_sets"]["tft_s3_dashboard_runtime_ui"]
        self.assertFalse(any("derived_slot" in entry for entry in entries))
        for value in range(96, 101):
            self.assertEqual([32768 + value], [entry["value"] for entry in entries if entry.get("data_value_id") == value])

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

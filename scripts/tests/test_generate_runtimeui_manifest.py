import json
import runpy
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "generate_runtimeui_manifest.py"
MODULE = runpy.run_path(str(SCRIPT), init_globals={"Import": lambda name: None, "env": None})


class RuntimeUiActionManifestTests(unittest.TestCase):
    def collect(self, value):
        with tempfile.TemporaryDirectory() as directory:
            modules_root = Path(directory)
            text_dir = modules_root / "AlarmModule" / "text"
            text_dir.mkdir(parents=True)
            payload = {"moduleId": "alarms", "values": [value]}
            (text_dir / "runtimeui.json").write_text(json.dumps(payload), encoding="utf-8")
            return MODULE["_collect_entries_for_locale"](
                modules_root,
                "fr",
                {"Alarm": 9},
                {"Alarm": "alarms"},
                {"alarms": 9},
            )

    def test_uint32_button_action_is_preserved_for_alarm_acknowledgement(self):
        entries = self.collect({
            "valueId": 1,
            "key": "alarms.active_mask",
            "label": "Alarmes actives",
            "type": "uint32",
            "domain": "alarm",
            "display": "flags",
            "actions": [{
                "id": "acknowledge",
                "command": "alarms.reset",
                "presentation": "button",
                "input": {"name": "id", "type": "uint32"},
                "refreshDomains": ["alarm"],
            }],
        })

        self.assertEqual(901, entries[0]["id"])
        self.assertEqual("acknowledge", entries[0]["actions"][0]["id"])
        self.assertEqual(
            {"name": "id", "type": "uint32"},
            entries[0]["actions"][0]["input"],
        )
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "RuntimeUiManifest_Generated.h"
            MODULE["_write_header"](header, entries)
            generated = header.read_text(encoding="utf-8")
        self.assertIn('"alarms.reset"', generated)
        self.assertIn("RuntimeUiActionInputType::UInt32", generated)
        self.assertIn("kRuntimeUiActionManifestItemCount = 1U", generated)

    def test_action_with_unsupported_presentation_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "unsupported action presentation"):
            self.collect({
                "valueId": 1,
                "key": "alarms.active_mask",
                "label": "Alarmes actives",
                "type": "uint32",
                "domain": "alarm",
                "actions": [{
                    "id": "acknowledge",
                    "command": "alarms.reset_slot",
                    "presentation": "dialog",
                    "input": {"name": "slot", "type": "uint32"},
                }],
            })


if __name__ == "__main__":
    unittest.main()

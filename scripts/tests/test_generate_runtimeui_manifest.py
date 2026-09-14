import json
import runpy
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "generate_runtimeui_manifest.py"
MODULE = runpy.run_path(str(SCRIPT), init_globals={"Import": lambda name: None, "env": None})


class RuntimeUiActionManifestTests(unittest.TestCase):
    def dialog_value(self):
        return {
            "valueId": 5, "key": "pool.device_count", "label": "Devices",
            "type": "uint32", "domain": "equipements",
            "displayConfig": {"actionDialog": {
                "buttonText": "Reset", "title": "Reset counters", "description": "Reset periods",
                "inputLabel": "Device", "allLabel": "All devices",
                "successText": "Reset {target}", "optionsUrl": "/api/runtime/pooldevice_options",
                "inputAction": "reset", "allAction": "reset_all",
                "metricsLabel": "Running time / volume", "rowButtonText": "Reset",
                "allButtonText": "Reset all", "confirmationText": "Reset {target}?",
                "confirmationHint": "Totals preserved", "confirmButtonText": "Confirm",
                "detailsLabel": "Reset details", "countText": "{count} devices",
                "columns": [{"label": "Day", "durationKey": "running.day_s", "volumeKey": "injected.day_ml"}],
            }},
            "actions": [
                {"id": "reset", "command": "pooldevice.uptime.reset", "presentation": "button",
                 "input": {"name": "slot", "type": "uint32"}},
                {"id": "reset_all", "command": "pooldevice.uptime.reset_all", "presentation": "button"},
            ],
        }

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

    def test_target_dialog_preserves_typed_commands_and_source(self):
        entry = self.collect(self.dialog_value())[0]
        self.assertEqual("/api/runtime/pooldevice_options", entry["displayConfig"]["actionDialog"]["optionsUrl"])
        self.assertEqual({"name": "slot", "type": "uint32"}, entry["actions"][0]["input"])
        self.assertEqual({"type": "none"}, entry["actions"][1]["input"])

    def test_target_dialog_rejects_invalid_action_bindings(self):
        for field in ("inputAction", "allAction"):
            with self.subTest(field=field):
                value = self.dialog_value()
                value["displayConfig"]["actionDialog"][field] = "unknown"
                with self.assertRaisesRegex(RuntimeError, "actionDialog invalid .* binding"):
                    self.collect(value)
        value = self.dialog_value()
        value["actions"][0]["input"]["type"] = "bool"
        with self.assertRaisesRegex(RuntimeError, "actionDialog invalid inputAction binding"):
            self.collect(value)

    def test_target_dialog_rejects_external_options_source(self):
        value = self.dialog_value()
        value["displayConfig"]["actionDialog"]["optionsUrl"] = "https://example.com/devices"
        with self.assertRaisesRegex(RuntimeError, "actionDialog invalid optionsUrl"):
            self.collect(value)

    def test_counter_table_rejects_missing_or_invalid_columns(self):
        value = self.dialog_value()
        value["displayConfig"]["actionDialog"]["columns"] = []
        with self.assertRaisesRegex(RuntimeError, "actionDialog missing columns"):
            self.collect(value)
        value = self.dialog_value()
        value["displayConfig"]["actionDialog"]["columns"][0]["durationKey"] = "running[0]"
        with self.assertRaisesRegex(RuntimeError, "actionDialog invalid column durationKey"):
            self.collect(value)

    def test_alarm_dialog_preserves_date_states_and_eligibility(self):
        value = self.dialog_value()
        dialog = value["displayConfig"]["actionDialog"]
        dialog.update(eligibleKey="resettable", rowPresentation="text", destructive=False)
        dialog["columns"] = [
            {"label": "Triggered", "type": "datetime", "key": "triggeredAt"},
            {"label": "Condition", "type": "enum", "key": "condition", "states": [
                {"value": 0, "label": "Inactive", "tone": "success"},
                {"value": 2, "label": "Unknown", "tone": "warning"},
            ]},
        ]
        preserved = self.collect(value)[0]["displayConfig"]["actionDialog"]
        self.assertEqual(dialog, preserved)

    def test_alarm_dialog_rejects_duplicate_enum_states(self):
        value = self.dialog_value()
        value["displayConfig"]["actionDialog"]["columns"] = [{
            "label": "Condition", "type": "enum", "key": "condition", "states": [
                {"value": 0, "label": "Inactive", "tone": "success"},
                {"value": 0, "label": "Active", "tone": "danger"},
            ],
        }]
        with self.assertRaisesRegex(RuntimeError, "actionDialog invalid enum state"):
            self.collect(value)

    def test_switch_dialog_preserves_typed_target_and_rejects_invalid_binding(self):
        value = self.dialog_value()
        value["actions"].append({"id": "set_device", "command": "pooldevice.write", "presentation": "switch",
                                 "input": {"name": "value", "type": "bool"},
                                 "target": {"name": "slot", "type": "uint32"}})
        column = {"label": "On/Off", "type": "switch", "key": "actualOn", "action": "set_device",
                  "targetKey": "value", "eligibleKey": "controllable"}
        value["displayConfig"]["actionDialog"]["columns"].insert(0, column)
        entries = self.collect(value)
        self.assertEqual({"name": "slot", "type": "uint32"}, entries[0]["actions"][-1]["target"])
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "RuntimeUiManifest_Generated.h"
            MODULE["_write_header"](header, entries)
            self.assertIn('RuntimeUiActionInputType::Bool, "slot"', header.read_text())
        column["action"] = "reset"
        with self.assertRaisesRegex(RuntimeError, "invalid switch action binding"):
            self.collect(value)

    def test_action_target_rejects_duplicate_input_name(self):
        value = self.dialog_value()
        value["actions"][0]["target"] = {"name": "slot", "type": "uint32"}
        with self.assertRaisesRegex(RuntimeError, "invalid action target"):
            self.collect(value)


if __name__ == "__main__":
    unittest.main()

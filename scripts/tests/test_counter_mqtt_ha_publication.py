#!/usr/bin/env python3
"""Static integration contract: manual counter updates reuse the MQTT / HA data value."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class CounterPublicationTests(unittest.TestCase):
    def test_pulse_count_default_ha_and_mqtt_identity(self):
        rows = json.loads((ROOT / "src/Core/Values/catalog.json").read_text(encoding="utf-8"))["values"]
        header = (ROOT / "src/Core/Generated/DataValueCatalog_Generated.h").read_text(encoding="utf-8")
        by_ref = {item["reference"]: item for item in rows}
        # Values 32..44 already existed; do not invent parallel count or total sensors.
        for slot in range(13):
            ref = f"i{slot:02}.count"
            value_id = 32 + slot
            with self.subTest(ref=ref):
                item = by_ref[ref]
                self.assertEqual(item["id"], value_id)
                self.assertTrue(item["ha_default"])
                self.assertEqual(item["topic"], f"rt/value/{value_id}")
                self.assertEqual(item["kind"], "counter")
                pattern = re.compile(
                    rf'\{{{value_id}, "{re.escape(ref)}", [^\n]+DataValueKind::Counter, '
                    rf'{slot}, true, "data_{value_id}", "rt/value/{value_id}"\}},'
                )
                self.assertRegex(header, pattern)

    def test_change_notification_to_mqtt_and_home_assistant(self):
        source = (ROOT / "src/Modules/IOModule/IOModule.cpp").read_text(encoding="utf-8")
        registry = (ROOT / "src/Core/Values/ValueRegistry.cpp").read_text(encoding="utf-8")
        store = (ROOT / "src/Core/DataStore/DataStore.h").read_text(encoding="utf-8")
        module = (ROOT / "src/Modules/Stores/DataStoreModule/DataStoreModule.cpp").read_text(encoding="utf-8")
        mqtt = (ROOT / "src/Modules/Network/MQTTModule/MQTTLifecycle.cpp").read_text(encoding="utf-8")
        assert 'values.write(countId, count' in source
        assert 'generation != slot.runtime.generation' in registry
        assert 'notification_(notificationContext_, id' in registry
        assert 'self->notifyChanged(DataKeys::ValueBase + id);' in store
        assert '_store.publishHa(d.id)' in module
        assert 'key == DataKeys::ValueBase + d->id' in module
        assert 'subscribe(EventId::DataChanged' in mqtt

if __name__ == "__main__":
    unittest.main()

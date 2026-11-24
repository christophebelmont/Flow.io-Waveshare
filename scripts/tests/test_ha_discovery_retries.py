#!/usr/bin/env python3
"""Source-level regression guards for the HA one-shot discovery fallback.

Run: python3 -m unittest scripts/tests/test_ha_discovery_retries.py
This verifies wiring and safety constraints; it does not replace an ESP32-S3
firmware build, MQTT broker test or Web UI test in a real browser.
"""
from pathlib import Path
import json
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


class OneShotDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.cpp = source("src/Modules/Network/HAModule/HAModule.cpp")
        self.header = source("src/Modules/Network/HAModule/HAModule.h")

    def test_bounded_retry_and_ready_entities_first(self):
        self.assertIn("DiscoveryRetryPasses = 4U", self.header)
        self.assertIn("DiscoveryRetryDelayMs = 5000U", self.header)
        self.assertIn("if (!anyReadyPending_() && anyPending_())", self.cpp)
        self.assertIn("(int32_t)(nowMs - deferredRetryNotBeforeMs_) < 0", self.cpp)
        self.assertIn("if (retryPass_ >= DiscoveryRetryPasses)", self.cpp)
        self.assertIn("memset(deferredBits_, 0, sizeof(uint32_t) * HA_PENDING_WORDS)", self.cpp)

    def test_failed_build_deferred_without_mqtt_job_leak(self):
        self.assertIn("deferredBuildId_ = messageId;", self.cpp)
        self.assertIn("return MqttBuildResult::NoLongerNeeded;", self.cpp)
        self.assertIn("const bool deferred = deferredBuildId_ == messageId;", self.cpp)
        self.assertIn("deferredBits_[messageId / 32U] |=", self.cpp)
        self.assertIn("if (!readAvailableDataValue(", self.cpp)

    def test_registered_offline_sensors_still_get_discovered(self):
        self.assertIn("dataValueCompatible(*descriptor, metadata)", self.cpp)
        self.assertIn("if (retryPass_ < DiscoveryRetryPasses)", self.cpp)
        self.assertIn("if (!dsSvc_->store->publishHa(id))", self.cpp)

    def test_psram_first_and_one_shot_release(self):
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", self.cpp)
        self.assertIn("HA_PENDING_WORDS * 2U", self.cpp)
        self.assertIn("heap_caps_free(pendingBits_);", self.cpp)
        self.assertIn("deferredBits_ = nullptr;", self.cpp)
        self.assertIn("heap_caps_free(mqttJson);", self.cpp)
        self.assertIn("SpiRamJsonDocument doc(512);", self.cpp)
        self.assertNotIn("char mqttJson[Limits::Mqtt::Buffers::StateCfg]", self.cpp)
        self.assertIn("oneShotResourcesReleased_ = true;", self.cpp)

    def test_refresh_requires_rebuild_after_one_shot(self):
        web = source("src/Modules/Network/WebInterfaceModule/WebInterfaceServer.cpp")
        ui = source("data/webinterface/app.js")
        self.assertIn('server_.on("/api/ha/discovery/refresh", HTTP_POST', web)
        self.assertIn("restart_required", web)
        self.assertIn("oneShotCompleted_.load()", self.cpp)
        self.assertIn("flowCfgCurrentModule === 'ha'", ui)
        self.assertIn("buildHaDiscoveryRefreshField", ui)
        self.assertIn("cfgdocs.ha.refresh.rebootConfirm", ui)
        self.assertIn("'/api/system/reboot'", ui)

    def test_action_localized_but_not_config_variable(self):
        ui = source("data/webinterface/app.js")
        action = ui.split("function buildHaDiscoveryRefreshField(config)", 1)[1].split(
            "function renderFlowCfgFieldsWithExtensions", 1
        )[0]
        self.assertNotIn("dataset.key", action)
        keys = [
            "cfgdocs.ha.refresh.label",
            "cfgdocs.ha.refresh.button",
            "cfgdocs.ha.refresh.rebootConfirm",
            "cfgdocs.ha.refresh.accepted",
        ]
        for lang in ("fr", "en"):
            translation = json.loads(source(
                f"src/Modules/Network/HAModule/text/i18n.{lang}.json"
            ))["translations"]
            for key in keys:
                with self.subTest(language=lang, key=key):
                    self.assertIn(key, translation)


if __name__ == "__main__":
    unittest.main()

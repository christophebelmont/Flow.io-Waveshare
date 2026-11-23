"""Regression: transient Wi-Fi loss must not turn a configured STA into captive AP.

Compiles the actual production NetworkManager declaration with isolated fakes.
Run: python -m unittest scripts/tests/test_wifi_sta_ap_fallback.py
No Arduino, network, or ESP32 hardware is required for these state transitions.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/Modules/Network/WifiProvisioningModule/WifiProvisioningModule.h"


class WifiStaApFallbackTests(unittest.TestCase):
    def test_network_manager_keeps_sta_after_transient_disconnection(self):
        source = HEADER.read_text(encoding="utf-8")
        start = source.index("namespace FlowNetwork {")
        finish = source.index("}  // namespace FlowNetwork", start)
        production = source[start:finish + len("}  // namespace FlowNetwork")]

        harness = r'''
#include <cassert>
#include <cstdint>
#define ETH_TIMEOUT_MS 7000UL
#define WIFI_TIMEOUT_MS 12000UL

enum class NetworkPortalReason : uint8_t {
    None = 0,
    MissingCredentials = 1,
    ConnectTimeout = 2
};
static uint32_t fakeNow = 0;
uint32_t millis() { return fakeNow; }

''' + production + r'''

int main() {
    using FlowNetwork::NetworkManager;
    using Reason = NetworkPortalReason;

    // First boot with saved credentials: a failed initial connection may
    // still start AP after the normal Ethernet + Wi-Fi startup timeout.
    NetworkManager cold;
    cold.begin(0);
    cold.updateConfig(true, true, true);
    cold.updateInterfaces(false, false);
    assert(cold.portalReason(6999) == Reason::None);
    assert(cold.portalReason(18999) == Reason::None);
    assert(cold.portalReason(19000) == Reason::ConnectTimeout);

    // Once the device has successfully obtained an IP address, no temporary
    // outage may force it into AP and disable STA reconnection.
    fakeNow = 20000;
    cold.updateInterfaces(false, true);
    assert(cold.portalReason(20000) == Reason::None);
    fakeNow = 27000;
    cold.updateInterfaces(false, false);
    assert(cold.portalReason(27000) == Reason::None);
    assert(cold.portalReason(34001) == Reason::None);
    assert(cold.portalReason(120000) == Reason::None);

    // If Wi-Fi is deliberately disabled or its credentials removed, AP must
    // remain an available provisioning path.
    cold.updateConfig(true, false, false);
    assert(cold.portalReason(120001) == Reason::MissingCredentials);
    cold.updateConfig(true, true, false);
    assert(cold.portalReason(120002) == Reason::MissingCredentials);

    // Restoring configured credentials resumes STA-only recovery after a
    // previously usable network, without a new AP transition.
    cold.updateConfig(true, true, true);
    assert(cold.portalReason(120003) == Reason::None);

    // Normal provisioning for a device with no prior network stays intact.
    NetworkManager unconfigured;
    unconfigured.begin(0);
    unconfigured.updateConfig(false, true, false);
    unconfigured.updateInterfaces(false, false);
    assert(unconfigured.portalReason(0) == Reason::MissingCredentials);

    NetworkManager firstBoot;
    firstBoot.begin(0);
    firstBoot.updateConfig(false, true, true);
    firstBoot.updateInterfaces(false, false);
    assert(firstBoot.portalReason(11999) == Reason::None);
    assert(firstBoot.portalReason(12000) == Reason::ConnectTimeout);

    // Ethernet connectivity still prevents provisioning regardless of STA.
    NetworkManager ethernet;
    ethernet.begin(0);
    ethernet.updateConfig(true, true, true);
    ethernet.updateInterfaces(true, false);
    assert(ethernet.portalReason(40000) == Reason::None);
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = pathlib.Path(folder) / "wifi_portal_policy.cpp"
            program = pathlib.Path(folder) / "wifi_portal_policy"
            src.write_text(harness, encoding="utf-8")
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(src), "-o", str(program)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(program)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()

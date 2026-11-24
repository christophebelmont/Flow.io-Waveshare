"""Exercise equipment startup policies, persistence and real drivers against fake hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_boot_binding import function

ROOT = Path(__file__).resolve().parents[2]


class PoolRelayBootTests(unittest.TestCase):
    def build_source(self):
        module = ROOT / 'src/Modules/PoolDeviceModule'
        header = (module / 'PoolDeviceModule.h').read_text()
        source = (ROOT / 'test/host/pool_relay_boot.cpp').read_text()
        for marker, signature in (
            ('// DEVICE_TYPE', 'enum PoolDeviceType'),
            ('// DEVICE_DEFINITION', 'struct PoolDeviceDefinition'),
            ('// DEVICE_SLOT', 'struct PoolDeviceSlot'),
        ):
            source = source.replace(marker, function(header, signature) + ';')
        runtime = (module / 'PoolDeviceRuntime.cpp').read_text()
        production = '\n'.join(function(runtime, signature) for signature in (
            'void PoolDeviceModule::commitRuntimeState_', 'bool PoolDeviceModule::configureRuntime_',
            'bool PoolDeviceModule::snapshotRouteFromIndex_', 'bool PoolDeviceModule::slotRuntimePublishable_',
            'bool PoolDeviceModule::buildStateSnapshot_', 'uint32_t PoolDeviceModule::runtimeSnapshotRefreshMs'))
        runtime_helpers = (module / 'PoolDeviceRuntime.h').read_text()
        source = source.replace('// RUNTIME_STATE_HELPERS', '\n'.join(
            function(runtime_helpers, 'static inline bool ' + name) for name in (
                'poolDeviceRuntimeState', 'poolDeviceRuntimeMetrics', 'samePoolDeviceTarget',
                'samePoolDeviceFeedback', 'samePoolDeviceState', 'setPoolDeviceRuntimeState',
                'setPoolDeviceRuntimeMetrics')))
        control = (module / 'PoolDeviceControl.cpp').read_text()
        for signature in ('void PoolDeviceModule::tickDevices_',
                          'bool PoolDeviceModule::dependenciesSatisfied_',
                          'bool PoolDeviceModule::maxUptimeReached_',
                          'uint32_t PoolDeviceModule::toSeconds_',
                          'PoolDeviceSvcStatus PoolDeviceModule::svcSetWritesEnabledImpl_'):
            production += '\n' + function(control, signature)
        production += '\n' + function(control, 'PoolDeviceSvcStatus PoolDeviceModule::setTarget_')
        startup = (module / 'PoolDeviceStartup.cpp').read_text()
        production += '\n' + startup[startup.index('bool PoolDeviceModule::restoreRunning_'):]
        source = source.replace('// PRODUCTION_FUNCTIONS', production)
        return source, module

    def compile_and_run(self, source, module):
        with tempfile.TemporaryDirectory(prefix='flow-relay-boot-') as directory:
            main, binary = Path(directory) / 'main.cpp', Path(directory) / 'test'
            main.write_text(source)
            for command in (
                ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-Itest/host/stubs', '-Itest/host', '-Isrc', '-Iinclude',
                 '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', str(main),
                 str(module / 'Drivers/PoolDeviceDriver.cpp'),
                 'src/Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.cpp',
                 'src/Modules/IOModule/IOProtocols/Modbus/AquaGemRtuCodec.cpp', '-o', str(binary)],
                [str(binary)],
            ):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_equipment_boot_and_safety(self):
        self.compile_and_run(*self.build_source())

    def test_manual_control_obeys_poollogic_ownership(self):
        source, module = self.build_source()
        source = source[:source.index('void allDriverPolicies()')] + """int main()
{
    PoolDeviceModule device;
    assert(device.configureRuntime_());
    assert(device.svcSetWritesEnabledImpl_(1) == POOLDEV_SVC_OK);
    PoolConfigurationService configuration{};
    bool allowed = false;
    configuration.ctx = &allowed;
    configuration.manualControlAllowed = [](void* ctx, uint8_t) { return *static_cast<bool*>(ctx); };
    device.poolConfigurationSvc_ = &configuration;
    const auto previous = device.slots_[0].desired;
    const PoolDeviceTarget on{true, 100};
    const PoolDeviceTarget off{false, 100};
    assert(device.setTarget_(0, &on, true) == POOLDEV_SVC_ERR_INTERLOCK);
    assert(device.setTarget_(0, &off, true) == POOLDEV_SVC_ERR_INTERLOCK);
    assert(device.slots_[0].desired.running == previous.running);
    allowed = true;
    assert(device.setTarget_(0, &on, true) == POOLDEV_SVC_OK);
    assert(device.slots_[0].desired.running);
    assert(device.setTarget_(0, &off, true) == POOLDEV_SVC_OK);
    allowed = false;
    // Controller writes remain valid in Auto/Hivers; only direct manual writes are gated.
    assert(device.setTarget_(0, &on, false) == POOLDEV_SVC_OK);
    assert(device.slots_[0].desired.running);
}
"""
        self.compile_and_run(source, module)


if __name__ == '__main__':
    unittest.main()

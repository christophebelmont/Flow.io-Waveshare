"""Exercise production stack sampling and explicit capture with deterministic task lifetimes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    start = source.index('SystemMonitorModule::' + name + '(')
    start = source.rfind('\n', 0, start) + 1
    return source[start:source.index('\n}', start) + 2]


class SysmonStackSamplingTests(unittest.TestCase):
    def test_mqtt_stop_waits_for_the_stack_read(self):
        source = (ROOT / 'src/Modules/Network/MQTTModule/MQTTTransport.cpp').read_text()
        production = []
        for name in ('acquireClientTaskStack_', 'releaseClientTaskStack_', 'stopClient_'):
            start = source.index('MQTTModule::' + name + '(')
            start = source.rfind('\n', 0, start) + 1
            production.append(source[start:source.index('\n}', start) + 2])
        fixture = (ROOT / 'test/host/mqtt_stack_source.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='flow-mqtt-stack-') as tmp:
            cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            cpp.write_text(fixture.replace('// PRODUCTION_METHODS', '\n\n'.join(production)))
            subprocess.run([
                'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-Itest/host/task_health_stubs',
                '-Itest/host/value_stubs', '-Isrc', str(cpp), '-o', str(exe)
            ], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)

    def test_sampling_and_explicit_capture(self):
        source = (ROOT / 'src/Modules/System/SystemMonitorModule/SystemMonitorModule.cpp').read_text()
        production = '\n\n'.join(method(source, name) for name in (
            'readTaskStack_', 'sampleTaskStack_', 'readCore1Runtime_', 'requestTaskCapture_', 'captureTasksIfRequested_'))
        fixture = (ROOT / 'test/host/sysmon_stack_sampling.cpp').read_text()
        with tempfile.TemporaryDirectory(prefix='flow-sysmon-') as tmp:
            cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            cpp.write_text(fixture.replace('// PRODUCTION_METHODS', production))
            subprocess.run([
                'c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-Itest/host/task_health_stubs',
                '-Itest/host/value_stubs', '-Isrc', str(cpp),
                'src/Core/TaskHealth.cpp', '-o', str(exe)
            ], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Compile real deterministic runtime/driver sources with a simulated PCNT peripheral."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
class ValueTests(unittest.TestCase):
    def test_history_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'history-json'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined,address',
                '-I.pio/libdeps/Flowio-waveshare-esp32-s3/ArduinoJson/src', '-Isrc', '-Iinclude',
                'test/host/history_json.cpp', '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_eventbus_startup(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'eventbus'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined,address', '-g', '-pthread',
                '-DEVENTBUS_PROFILE=0', '-DFLOW_ENABLE_BUFFER_USAGE_TRACKING=0',
                '-Itest/host/eventbus_stubs', '-Itest/host/value_stubs', '-Isrc', '-Iinclude',
                'test/host/eventbus_startup.cpp', 'src/Core/EventBus/EventBus.cpp',
                '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)

    def test_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'values'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-variable', '-fsanitize=undefined,address', '-g', '-pthread',
                '-Itest/host/value_stubs', '-Isrc', '-Iinclude',
                'test/host/values.cpp', 'src/Core/Values/ValueRegistry.cpp',
                'src/Modules/IOModule/IODrivers/PcntCounterDriver.cpp',
                'src/Modules/PoolHistoryModule/ValueHistory.cpp', '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], cwd=ROOT, check=True)
if __name__ == '__main__':
    unittest.main()

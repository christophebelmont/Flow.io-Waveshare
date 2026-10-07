"""Execute production MQTT scheduling/dispatch methods with deterministic host fakes.

Only hardware, time and producer callbacks are replaced. Interleavings are injected
while the actual dispatcher releases its lock to build/publish a message.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'src/Modules/Network/MQTTModule'


def method(source, name):
    start = source.index('MQTTModule::' + name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


class MqttQueueTests(unittest.TestCase):
    def test_scheduling_and_dispatch(self):
        header = (MODULE / 'MQTTModule.h').read_text()
        source = (MODULE / 'MQTTQueue.cpp').read_text()
        names = ['findJobSlot_', 'allocJobSlot_', 'queuePush_', 'queuePop_',
                 'queueSlot_', 'deferJob_', 'releaseJob_', 'retryPendingJobsNoLock_',
                 'snapshotQueueStatsNoLock_', 'logQueueSnapshot_', 'logEnqueueIssue_', 'enqueueJob_',
                 'enqueue', 'dequeueNextJob_', 'processJobs_', 'updateAndReportQueueOccupancy_']
        bodies = [method(source, name) for name in names]
        declarations = '\n'.join(body[:body.index('\n{')].replace('MQTTModule::', '') + ';'
                                 for body in bodies)
        types = header[header.index('    enum class JobState'):header.index('    struct ScratchBuffers')]
        fixture = (ROOT / 'test/host/mqtt_queue.cpp').read_text()
        code = fixture.replace('// PRODUCTION_TYPES', types).replace('// PRODUCTION_DECLARATIONS', declarations)
        code = code.replace('// PRODUCTION_METHODS', '\n\n'.join(bodies))
        with tempfile.TemporaryDirectory(prefix='flow-mqtt-queue-') as tmp:
            cpp = Path(tmp) / 'mqtt_queue.cpp'
            cpp.write_text(code)
            binary = Path(tmp) / 'test'
            for command in (['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                             '-Isrc', str(cpp), '-o', str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if result.stdout:
                    print(result.stdout, end='')


if __name__ == '__main__':
    unittest.main()

"""Exercise production configuration event/gate/tick methods with fake transport."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'src/Modules/Network/MQTTModule'

class DeferredConfigTests(unittest.TestCase):
    def test_config_deferred(self):
        source = (MODULE / 'MqttConfigRouteProducer.cpp').read_text()
        names = ['requestFullSync', 'refreshReadyGateAndMaybeSync_', 'onTransportTick_',
                 'onEvent_', 'routePriority_', 'offerBackground_', 'enqueueByRoute_',
                 'setPending_', 'isPending_', 'findRouteByMessage_', 'onMessagePublished_', 'onMessageDropped_']
        bodies = []
        for name in names:
            start = source.index('MqttConfigRouteProducer::' + name + '(')
            start = source.rfind('\n', 0, start) + 1
            bodies.append(source[start:source.index('\n}', start) + 2])
        header = (MODULE / 'MqttConfigRouteProducer.h').read_text()
        route = header[header.index('    struct Route {'):header.index('\n    /**', header.index('    struct Route {'))]
        fixture = (ROOT / 'test/host/mqtt_config_deferred.cpp').read_text()
        code = fixture.replace('// ROUTE', route).replace('// METHODS', '\n\n'.join(bodies))
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'test.cpp'
            cpp.write_text(code)
            binary = Path(tmp) / 'test'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-pthread', '-Itest/host/value_stubs',
                '-Isrc', '-Iinclude', str(cpp), '-o', str(binary)], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()

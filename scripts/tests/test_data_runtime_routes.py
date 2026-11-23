"""Exercise catalogue route startup with the production runtime MQTT producer."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_io_discovery_boot import function

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / 'src/Modules/Network/MQTTModule'


class DataRuntimeRoutesTests(unittest.TestCase):
    def test_unconfigured_counters_do_not_block_io_startup(self):
        source = (ROOT / 'src/Modules/Stores/DataStoreModule/DataStoreModule.cpp').read_text()
        names = ['onConfigLoaded', 'onStart', 'runtimeSnapshotCount', 'runtimeSnapshotSuffix',
                 'runtimeSnapshotAffectsKey', 'buildRuntimeSnapshot', 'route_']
        methods = []
        for name in names:
            marker = 'DataStoreModule::' + name + '('
            start = source.index(marker)
            signature = source[source.rfind('\n', 0, start) + 1:start] + marker
            methods.append(function(source, signature))
        declarations = '\n'.join(
            body[:body.index('{')].replace('DataStoreModule::', '').rstrip() +
            (' override;' if 'DataStoreModule::runtimeSnapshot' in body or
             'DataStoreModule::buildRuntimeSnapshot' in body else ';')
            for body in methods)
        runtime_header = (MODULE / 'RuntimeProducer.h').read_text()
        runtime_class = runtime_header[runtime_header.index('class RuntimeProducer {'):]
        runtime_class = runtime_class.replace('private:', 'public:')
        runtime_source = (MODULE / 'RuntimeProducer.cpp').read_text()
        runtime_methods = runtime_source[runtime_source.index('namespace {'):]
        fixture = (ROOT / 'test/host/data_runtime_routes.cpp').read_text()
        code = fixture.replace('// DATA_METHOD_DECLARATIONS', declarations)
        code = code.replace('// DATA_METHODS', '\n'.join(methods))
        code = code.replace('// RUNTIME_CLASS', runtime_class).replace('// RUNTIME_METHODS', runtime_methods)
        with tempfile.TemporaryDirectory(prefix='flow-data-runtime-') as tmp:
            cpp = Path(tmp) / 'test.cpp'
            exe = Path(tmp) / 'test'
            cpp.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-Itest/host/value_stubs',
                            '-Isrc', '-Iinclude', str(cpp),
                            'src/Core/Values/ValueRegistry.cpp', 'src/Core/Values/ValueExpression.cpp',
                            '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

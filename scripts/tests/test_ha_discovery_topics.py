"""Build production HA topics in the actual MQTT scratch-buffer declaration."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_ha_alarm_discovery import function

ROOT = Path(__file__).resolve().parents[2]


class HaDiscoveryTopicTests(unittest.TestCase):
    def test_registered_and_maximum_length_topics(self):
        ha = ROOT / 'src/Modules/Network/HAModule'
        header = (ha / 'HAModule.h').read_text()
        source = (ha / 'HAModule.cpp').read_text()
        mqtt = (ROOT / 'src/Modules/Network/MQTTModule/MQTTModule.h').read_text()
        config = header[header.index('    struct HAConfig {'):]
        config = config[:config.index('\n    };') + len('\n    };')]
        fields = '\n'.join(line for line in header.splitlines() if any(
            key in line for key in ('HAConfig cfgData_', 'char nodeTopicId_[', 'char objectIdBuf_[')))
        assertion = header[header.index('    static_assert(Limits::Mqtt::Buffers::DynamicTopic'):]
        assertion = assertion[:assertion.index(';') + 1]
        scratch = mqtt[mqtt.index('    struct ScratchBuffers {'):]
        scratch = scratch[:scratch.index('\n    };') + len('\n    };')]
        code = r'''
#include "Core/SystemLimits.h"
#include "Core/Services/IMqtt.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#define FLOW_WIRDEF_HA_ENTITY_PREFIX "fio"
#define LOGW(...) ((void)0)
'''
        code += function(source, 'static bool formatChecked(')
        code += '\nstruct HAModule {\n' + config + fields + assertion
        code += '\nbool publishDiscovery(const char*, const char*, MqttBuildContext&);\n};\n'
        code += scratch + '\n' + function(source, 'bool HAModule::publishDiscovery(')
        code += r'''
int main() {
    HAModule module;
    ScratchBuffers scratch;
    strcpy(module.nodeTopicId_, "esp32_abcdef");
    strcpy(scratch.payload, "{}");
    MqttBuildContext ctx{};
    ctx.topic = scratch.topic; ctx.topicCapacity = sizeof(scratch.topic);
    ctx.payload = scratch.payload; ctx.payloadCapacity = sizeof(scratch.payload);
    for (unsigned port = 502; port <= 504; ++port) {
        for (const auto* component : {"number", "select"}) {
            for (const auto* field : {"device_id", "timeout_ms", "stop_bits", "error_mode"}) {
                const std::string object = "fio_modbus_tcp_" + std::to_string(port) + "_" + field;
                assert(module.publishDiscovery(component, object.c_str(), ctx));
                const std::string expected = "homeassistant/" + std::string(component) +
                    "/esp32_abcdef/" + object + "/config";
                assert(std::string(ctx.topic) == expected);
                assert(ctx.topicLen == expected.size() && ctx.payloadLen == 2);
                assert(ctx.retain && ctx.qos == 0);
            }
        }
    }
    const std::string prefix(sizeof(module.cfgData_.discoveryPrefix) - 1, 'p');
    const std::string node(sizeof(module.nodeTopicId_) - 1, 'n');
    const std::string object(sizeof(module.objectIdBuf_) - 1, 'o');
    strcpy(module.cfgData_.discoveryPrefix, prefix.c_str());
    strcpy(module.nodeTopicId_, node.c_str());
    assert(module.publishDiscovery("binary_sensor", object.c_str(), ctx));
    assert(std::string(ctx.topic) == prefix + "/binary_sensor/" + node + "/" + object + "/config");
    assert(ctx.topicLen == strlen(ctx.topic));
    char shortTopic[70]{};
    ctx.topic = shortTopic; ctx.topicCapacity = sizeof(shortTopic);
    assert(!module.publishDiscovery("binary_sensor", object.c_str(), ctx));
}
'''
        with tempfile.TemporaryDirectory(prefix='flow-ha-topics-') as tmp:
            cpp = Path(tmp) / 'topics.cpp'
            cpp.write_text(code)
            exe = Path(tmp) / 'topics'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-Isrc', '-Iinclude', str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

"""Exercise real HA registration paths, including first allocation and PSRAM failure."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from scripts.tests.test_io_discovery_boot import function

ROOT = Path(__file__).resolve().parents[2]
KINDS = [('Sensor', 'sensor', 'sensors', 'SENSORS'),
         ('BinarySensor', 'binarySensor', 'binarySensors', 'BINARY_SENSORS'),
         ('Switch', 'switch', 'switches', 'SWITCHES'),
         ('Number', 'number', 'numbers', 'NUMBERS'),
         ('Button', 'button', 'buttons', 'BUTTONS'),
         ('Select', 'select', 'selects', 'SELECTS')]

class HaDynamicStorageTests(unittest.TestCase):
    def test_all_entity_types(self):
        source = (ROOT/'src/Modules/Network/HAModule/HAModule.cpp').read_text()
        production = 'template<typename T>\n' + function(source, 'bool reserveDiscoveryEntries(')
        production += '\n' + function(source, 'bool HAModule::ensureStorage_(')
        for title, _, _, _ in KINDS:
            production += '\n' + function(source, f'bool HAModule::add{title}Entry(')
        production += '\n' + function(source, 'bool HAModule::addDiscoveryRemovalSvc_(')
        production += '\n' + function(source, 'bool HAModule::buildEntityMessage_(')
        preamble = r'''
#include "Core/Services/IHA.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>
#include <string>
#define LOGW(...) ((void)0)
enum class TrackedBufferId { HaEntityTables };
struct BufferUsageTracker { template<typename... T> static void note(T...) {} };
struct MqttBuildContext {};
struct FakeMqtt {
    void* ctx=nullptr;
    void (*formatTopic)(void*,const char*,char*,size_t)=[](void*,const char*,char* out,size_t){out[0]=0;};
};
struct HAModule {
    FakeMqtt mqtt; FakeMqtt* mqttSvc_=&mqtt;
    char objectIdBuf_[64]{}, stateTopicBuf_[64]{}, commandTopicBuf_[64]{};
    bool buildObjectId(const char*,char*,size_t){return true;}
    template<typename... T> bool publishSensor(T...){return true;}
    template<typename... T> bool publishBinarySensor(T...){return true;}
    template<typename... T> bool publishSwitch(T...){return true;}
    template<typename... T> bool publishNumber(T...){return true;}
    template<typename... T> bool publishButton(T...){return true;}
    template<typename... T> bool publishSelect(T...){return true;}
    bool buildEntityMessage_(uint16_t,MqttBuildContext&);
    bool oneShotCompleted_=false, oneShotResourcesReleased_=false;
    uint32_t* pendingBits_=nullptr;
    static constexpr size_t HA_PENDING_WORDS=128;
    unsigned refreshes=0;
    void requestAutoconfigRefresh(){++refreshes;}
    size_t entityTableUsedBytes_() const{return 0;}
    size_t entityTableCapacityBytes_() const{return 0;}
    bool ensureStorage_();
    static constexpr uint16_t MAX_HA_REMOVALS=300;
    HADiscoveryRemovalEntry* removals_=nullptr;
    uint16_t removalCount_=0, removalCapacity_=0;
    bool addDiscoveryRemovalSvc_(const HADiscoveryRemovalEntry*);
'''
        for title, singular, plural, upper in KINDS:
            preamble += f'''
    static constexpr uint16_t MAX_HA_{upper}=300;
    HA{title}Entry* {plural}_=nullptr;
    uint16_t {singular}Count_=0, {singular}Capacity_=0;
    bool add{title}Entry(const HA{title}Entry&);
'''
        preamble += '~HAModule(){heap_caps_free(pendingBits_);heap_caps_free(removals_);'
        for _, _, plural, _ in KINDS:
            preamble += f'heap_caps_free({plural}_);'
        preamble += '}};\n'
        scenarios = r'''
int main(){
    std::string keys[301];
    for(unsigned i=0;i<301;++i)keys[i]="entity_"+std::to_string(i);
'''
        common = ('ownerId', 'objectSuffix', 'name')
        fields = {
            'Sensor': ('stateTopicSuffix','valueTemplate'),
            'BinarySensor': ('stateTopicSuffix','valueTemplate'),
            'Switch': ('stateTopicSuffix','valueTemplate','commandTopicSuffix','payloadOn','payloadOff'),
            'Number': ('stateTopicSuffix','valueTemplate','commandTopicSuffix','commandTemplate'),
            'Button': ('commandTopicSuffix','payloadPress'),
            'Select': ('stateTopicSuffix','valueTemplate','commandTopicSuffix','commandTemplate','optionsJson')}
        for title, singular, plural, upper in KINDS:
            scenarios += f'{{ HAModule module; HA{title}Entry entry{{}};\n'
            for field in common + fields[title]:
                scenarios += f'entry.{field}="test";\n'
            scenarios += f'''
    assert(module.{plural}_==nullptr);
    assert(module.ensureStorage_());
    fakePsramAvailable=false;
    assert(!module.add{title}Entry(entry));
    assert(module.{plural}_==nullptr && module.{singular}Count_==0 && module.refreshes==0);
    fakePsramAvailable=true;
    for(unsigned i=0;i<16;++i){{entry.objectSuffix=keys[i].c_str();assert(module.add{title}Entry(entry));}}
    assert(module.{singular}Count_==16);
    MqttBuildContext context;
    assert(module.buildEntityMessage_(0,context));
    assert(module.buildEntityMessage_(15,context));
    assert(!module.buildEntityMessage_(16,context));
    const auto* original=module.{plural}_;
    entry.objectSuffix=keys[16].c_str();
    fakePsramAvailable=false;
    assert(!module.add{title}Entry(entry));
    assert(module.{plural}_==original && module.{singular}Count_==16);
    assert(!strcmp(module.{plural}_[0].objectSuffix,keys[0].c_str()));
    fakePsramAvailable=true;
    for(unsigned i=16;i<300;++i){{entry.objectSuffix=keys[i].c_str();assert(module.add{title}Entry(entry));}}
    assert(module.{singular}Count_==300);
    entry.objectSuffix=keys[299].c_str();entry.name="updated";
    assert(module.add{title}Entry(entry));
    assert(module.{singular}Count_==300 && !strcmp(module.{plural}_[299].name,"updated"));
    entry.objectSuffix=keys[300].c_str();
    assert(!module.add{title}Entry(entry));
    assert(module.{singular}Count_==300);
}}
'''
        scenarios += r'''
    HAModule module;
    assert(module.ensureStorage_());
    HADiscoveryRemovalEntry removal{"sensor", "test"};
    fakePsramAvailable=false;
    assert(!module.addDiscoveryRemovalSvc_(&removal) && module.removalCount_==0);
    fakePsramAvailable=true;
    for(unsigned i=0;i<300;++i){removal.objectSuffix=keys[i].c_str();assert(module.addDiscoveryRemovalSvc_(&removal));}
    assert(module.addDiscoveryRemovalSvc_(&removal) && module.removalCount_==300);
    removal.objectSuffix=keys[300].c_str();
    assert(!module.addDiscoveryRemovalSvc_(&removal));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'ha.cpp'; exe=Path(tmp)/'ha'
            cpp.write_text(preamble+production+scenarios)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all','-Itest/host/value_stubs','-Isrc','-Iinclude',
                            str(cpp),'-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()

"""Exercise the production IO Discovery boot function with failing dependencies."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class DiscoveryBootTest(unittest.TestCase):
    def test_failures_return_and_preserve_other_entities(self):
        source = (ROOT / 'src/Profiles/Waveshare/WaveshareIoAssembly.cpp').read_text()
        production = '\n'.join(function(source, signature) for signature in (
            'bool reportDiscoveryResult(', 'void registerIoHomeAssistant('))
        harness = r'''
#include "Profiles/Waveshare/DerivedValueHaDiscovery.h"
#include <cassert>
#include <cstdarg>
#include <cstring>
using LogModuleId = int;
namespace LogModuleIdValue { constexpr int Core = 0; }
unsigned errors = 0, attempts = 0, accepted = 0, refreshed = 0;
bool memoryOk = true, assignmentsOk = true, rolesOk = true, roleEntitiesOk = true;
bool sensorsOk = true, removalsOk = true;
namespace Log { void error(int, const char*, ...) { ++errors; } }
struct Serial { void printf(const char*, ...) {} } serial;
namespace Board { namespace SerialMap { Serial& logSerial() { return serial; } } }
namespace Limits { namespace Io { constexpr int MaxPoolDevices = 16; } }
struct PoolDeviceAssignments { unsigned filtration{}, phPump{}, disinfectionPump{}, robot{}, filling{}, chlorineGenerator{}, heater{}; };
struct PoolConfigurationService {
    bool (*getDeviceAssignments)(void*, PoolDeviceAssignments*) = [](void*, PoolDeviceAssignments*) { return assignmentsOk; };
    void* ctx{};
};
HAService ha{};
PoolConfigurationService pool;
enum class ServiceId { Ha, PoolConfiguration };
struct Services {
    template<class T> T* get(ServiceId id) { return static_cast<T*>(id == ServiceId::Ha ? static_cast<void*>(&ha) : static_cast<void*>(&pool)); }
};
struct AppContext { Services services; int domainValue{}; int* domain = &domainValue; };
namespace PoolRoleHaDiscovery {
struct Storage {};
bool prepare(Storage&, const PoolDeviceAssignments&, int) { return rolesOk; }
bool registerEntries(const HAService&, Storage&) { return roleEntitiesOk; }
}
struct FlowIoDiscoveryHeap {
    char derivedUnit[ValueIds::DerivedCapacity][UnitTextCapacity]{};
    PoolRoleHaDiscovery::Storage poolRoles;
    DerivedValueHaDiscovery::Storage derivedValues;
} heap;
FlowIoDiscoveryHeap* gDiscoveryHeap = nullptr;
bool ensureDiscoveryHeap() { gDiscoveryHeap = memoryOk ? &heap : nullptr; return memoryOk; }
struct ModuleInstances {
    const HAService* haService{};
    struct IO { const char* derivedValueName(uint8_t) { return ""; } bool derivedValuePublished(uint8_t slot) { return slot % 2 == 0; } } ioModule;
};
void readValueDisplayUnit(const ModuleInstances&, ValueId, char* out, size_t) { out[0] = 0; }
unsigned analog = 0, digital = 0, switches = 0;
void syncAnalogSensors(ModuleInstances&) { ++analog; }
void syncDigitalInputBinarySensors(ModuleInstances&) { ++digital; }
void syncSwitches(int&, ModuleInstances&) { ++switches; }
'''
        scenarios = r'''
int main() {
    ha.addSensor = [](void*, const HASensorEntry*) { ++attempts; accepted += sensorsOk; return sensorsOk; };
    ha.addDiscoveryRemoval = [](void*, const HADiscoveryRemovalEntry*) { ++attempts; accepted += removalsOk; return removalsOk; };
    ha.requestRefresh = [](void*) { ++refreshed; return true; };
    AppContext ctx; ModuleInstances modules;
    constexpr unsigned slots = ValueIds::DerivedCapacity;
    constexpr unsigned sensors = (slots + 1) / 2, removals = slots / 2;
    // Missing allocation must return, never dereference the absent heap or halt.
    memoryOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(errors == 1 && attempts == 0 && refreshed == 0);
    memoryOk = true;
    // A rejected sensor must not prevent cleanup entries or unrelated IO entities.
    sensorsOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(attempts == slots && accepted == removals && errors == 1 + sensors);
    assert(analog == 1 && digital == 1 && switches == 1 && refreshed == 1);
    // Even all registration failures return and reach the remaining boot work.
    removalsOk = false; roleEntitiesOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(attempts == 2 * slots && accepted == removals && errors == 2 + sensors + slots && refreshed == 2);
    // Missing assignments skip role-dependent switches, preserving independent IO.
    assignmentsOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 3 && digital == 3 && switches == 2 && refreshed == 3);
    assignmentsOk = true; rolesOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 4 && switches == 2 && refreshed == 4);
    // A subsequent boot with resources restored registers all values normally.
    rolesOk = roleEntitiesOk = sensorsOk = removalsOk = true;
    registerIoHomeAssistant(ctx, modules);
    assert(accepted == removals + slots && analog == 5 && switches == 3 && refreshed == 5);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / 'boot.cpp'
            binary = pathlib.Path(tmp) / 'boot'
            cpp.write_text(harness + production + scenarios)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-Isrc', str(cpp), '-o', str(binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

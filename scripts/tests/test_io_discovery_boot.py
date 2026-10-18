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
#include "Core/Services/IHA.h"
#include <cassert>
#include <cstdarg>
#include <cstring>
using LogModuleId = int;
namespace LogModuleIdValue { constexpr int Core = 0; }
unsigned errors = 0, refreshed = 0;
bool memoryOk = true, assignmentsOk = true, rolesOk = true, roleEntitiesOk = true;
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
    PoolRoleHaDiscovery::Storage poolRoles;
} heap;
FlowIoDiscoveryHeap* gDiscoveryHeap = nullptr;
bool ensureDiscoveryHeap() { gDiscoveryHeap = memoryOk ? &heap : nullptr; return memoryOk; }
struct ModuleInstances {
    const HAService* haService{};
};
unsigned analog = 0, switches = 0;
void syncDataSensors(ModuleInstances&) { ++analog; }
void syncSwitches(int&, ModuleInstances&) { ++switches; }
'''
        scenarios = r'''
int main() {
    ha.requestRefresh = [](void*) { ++refreshed; return true; };
    AppContext ctx; ModuleInstances modules;
    // Missing allocation returns without registering entities.
    memoryOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(errors == 1 && refreshed == 0);
    memoryOk = true;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 1 && switches == 1 && refreshed == 1);
    // Rejected role entities do not prevent catalogue registration.
    roleEntitiesOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(errors == 2 && analog == 2 && refreshed == 2);
    assignmentsOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 3 && switches == 2 && refreshed == 3);
    assignmentsOk = true; rolesOk = false;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 4 && switches == 2 && refreshed == 4);
    rolesOk = roleEntitiesOk = true;
    registerIoHomeAssistant(ctx, modules);
    assert(analog == 5 && switches == 3 && refreshed == 5);
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

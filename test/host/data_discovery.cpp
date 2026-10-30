#include "Core/Services/IHA.h"
#include "Core/Values/DataValueView.h"
#include "Core/Values/InternalAnalogSources.h"
#include "Core/Values/PoolDeviceValues.h"
#include <cassert>
#include <esp_heap_caps.h>
#include <cstring>
#include <set>
#include <string>
struct TestStore {
    ValueRegistry values;
    bool enabled[ValueIds::Capacity]{};
    bool publishHa(ValueId id) const { return enabled[id]; }
};
struct ModuleInstances { TestStore* ioDataStore; HAService* haService; int ioModule=0; };
struct FlowIoDiscoveryHeap {
    struct DataLabel { char name[64]{}; char unit[UnitTextCapacity]{}; ValueId id=VALUE_INVALID; DataLabel* next=nullptr; };
    DataLabel* dataLabels=nullptr;
    ~FlowIoDiscoveryHeap() { while(dataLabels) { auto* next=dataLabels->next; heap_caps_free(dataLabels);dataLabels=next; } }
    char analogValueTpl[32][128]{};
} heap;
auto* gDiscoveryHeap=&heap;
bool ensureDiscoveryHeap() { return true; }
bool reportDiscoveryResult(bool result, const char*) { assert(result);return result; }
void buildAnalogValueTemplate(int&, uint8_t, char* out, size_t len) { snprintf(out,len,"{{ value_json.value }}"); }
// PRODUCTION_SYNC
int main() {
    TestStore store;
    assert(store.values.begin());
    assert(InternalAnalogSources::define(store.values));
    assert(PoolDeviceValues::define(store.values));
    PoolDeviceValues::setNames(store.values, 2, "Chlore");
    struct Captured { std::set<std::string> added, removed; } captured;
    HAService service{}; service.ctx=&captured;
    service.addSensor=[](void* ctx,const HASensorEntry* sensor) {
        auto& out=*static_cast<Captured*>(ctx); out.added.insert(sensor->objectSuffix);
        assert(sensor->dataValueId != VALUE_INVALID);
        if (strcmp(sensor->objectSuffix,"data_146")==0) {
            assert(strcmp(sensor->stateTopicSuffix,"rt/value/146")==0);
            assert(strcmp(sensor->unit,"s")==0);
            assert(strcmp(sensor->name,"Chlore · Temps de marche du jour")==0);
        }
        if (strcmp(sensor->objectSuffix,"data_116")==0)
            assert(strcmp(sensor->name,"Chlore · Volume restant")==0);
        if (strcmp(sensor->objectSuffix,"data_117")==0)
            assert(strcmp(sensor->name,"Chlore · Remplissage estimé")==0);
        return true;
    };
    service.addBinarySensor=[](void* ctx,const HABinarySensorEntry* sensor) {
        static_cast<Captured*>(ctx)->added.insert(sensor->objectSuffix);
        assert(sensor->dataValueId == ValueIds::DigitalState+4);
        assert(strcmp(sensor->stateTopicSuffix,"rt/value/164")==0);
        assert(sensor->availabilityTemplate != nullptr);
        return true;
    };
    service.addDiscoveryRemoval=[](void* ctx,const HADiscoveryRemovalEntry* removal) {
        assert(strcmp(removal->component,"sensor")==0 || strcmp(removal->component,"binary_sensor")==0);
        static_cast<Captured*>(ctx)->removed.insert(removal->objectSuffix); return true;
    };
    ModuleInstances modules{&store,&service};
    // First boot: only pd2 remaining volume and daily uptime requested.
    store.enabled[116]=store.enabled[146]=true;
    syncDataSensors(modules);
    assert((captured.added==std::set<std::string>{"data_116","data_146"}));
    assert(captured.removed.size()==kDataValueCount-2);
    assert(captured.removed.count("data_117"));
    // Next boot: disable uptime; its retained discovery must be removed.
    captured={}; store.enabled[146]=false;
    syncDataSensors(modules);
    assert((captured.added==std::set<std::string>{"data_116"}));
    assert(captured.removed.count("data_146"));
    captured={}; store.enabled[117]=true;
    syncDataSensors(modules);
    assert(captured.added.count("data_117"));
    store.enabled[117]=false;
    // All 16 uptimes can be requested independently, without tank configuration.
    captured={}; store.enabled[116]=false;
    for (uint8_t slot=0;slot<16;++slot) store.enabled[PoolDeviceValues::uptimeId(slot)]=true;
    syncDataSensors(modules);
    assert(captured.added.size()==16 && captured.removed.size()==kDataValueCount-16);
    captured={};
    ValueMetadata state{}; state.type=ValueType::Bool;
    assert(store.values.define(ValueIds::DigitalState+4, state));
    store.enabled[ValueIds::DigitalState+4]=true;
    syncDataSensors(modules);
    assert(captured.added.count("data_164"));
    assert(!captured.added.count("data_166") && !captured.added.count("data_168"));
    puts("Catalogue discovery: boot-time selection, deselection cleanup and all daily uptimes OK");
}

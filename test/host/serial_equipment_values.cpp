#include "Core/Values/SerialEquipmentValues.h"
#include "Modules/PoolHistoryModule/ValueHistory.h"
#include <esp_heap_caps.h>
#include <cassert>
#include <vector>
#include <cstdio>
struct Store {
    ValueRegistry values;
    bool defineDataValue(ValueId id, const ValueMetadata& metadata) { return values.define(id, metadata); }
};
int main() {
    Store store; assert(store.values.begin());
    const size_t emptyBytes = store.values.allocatedBytes();
    PoolDriverConfig pump{};
    pump.capabilities.kind = PoolControlKind::Rs485;
    pump.capabilities.unit = PoolSetpointUnit::Rpm;
    pump.serial.hasFeedback = true;
    pump.serial.staleMs = 5000;
    assert(SerialEquipmentValues::define(store, 15, "Pompe", pump));
    assert(store.values.used() == 3);
    const auto speed = SerialEquipmentValues::id(15, EquipmentMetric::Observed);
    const auto running = SerialEquipmentValues::id(15, EquipmentMetric::Running);
    assert(speed > 255);
    ValueSnapshot sample{}; ValueMetadata metadata{};
    assert(store.values.read(speed, sample, &metadata));
    assert(!metadata.history && metadata.usable && !strcmp(metadata.displayUnit,"rpm"));
    assert(!store.values.read(SerialEquipmentValues::id(14, EquipmentMetric::Observed), sample));
    const size_t pumpBytes = store.values.allocatedBytes() - emptyBytes;
    ValueExpression::Program formula{};
    assert(ValueExpression::compile("pd15.observed / 2", formula));
    const double parameters[ValueExpression::ParamCount]{};
    assert(store.values.defineProgram(ValueIds::Derived, formula, parameters, AggregationMode::Gauge));
    PoolDeviceTarget target{}; target.setpoint = 2500;
    PoolDeviceFeedback feedback{}; feedback.observed.setpoint=2400; feedback.observed.running=true;
    feedback.observedAtMs=100; feedback.observedValid=true; feedback.quality=PoolFeedbackQuality::Confirmed;
    const auto allocations = fakeHeapAllocations;
    SerialEquipmentValues::publish(store.values,15,pump,target,feedback,true,100,100);
    assert(store.values.read(speed,sample) && sample.quality==ValueQuality::Valid && sample.value.f==2400);
    assert(store.values.read(ValueIds::Derived,sample) && sample.value.d==1200);
    assert(store.values.read(running,sample) && sample.value.b);
    SerialEquipmentValues::publish(store.values,15,pump,target,feedback,true,5200,5200);
    assert(store.values.read(speed,sample) && sample.quality==ValueQuality::Invalid);
    assert(store.values.read(ValueIds::Derived,sample) && sample.quality==ValueQuality::Invalid);
    feedback.observedAtMs=5300;
    SerialEquipmentValues::publish(store.values,15,pump,target,feedback,true,5300,5300);
    assert(store.values.read(speed,sample) && sample.quality==ValueQuality::Valid);
    store.values.setUsable(speed,false);
    assert(store.values.read(ValueIds::Derived,sample) && sample.quality==ValueQuality::Invalid);
    assert(store.values.read(speed,sample) && sample.quality==ValueQuality::Valid);
    assert(fakeHeapAllocations==allocations);
    feedback.observedAtMs=UINT32_MAX-10;
    SerialEquipmentValues::publish(store.values,15,pump,target,feedback,true,20,(UINT64_C(1)<<32)+20);
    assert(store.values.read(speed,sample) && sample.quality==ValueQuality::Valid && sample.timestampMs==UINT32_MAX-10ULL);
    const auto registeredBeforeFailure=store.values.used();
    fakePsramAvailable=false;
    assert(!SerialEquipmentValues::define(store,14,"Absent",pump));
    assert(store.values.used()==registeredBeforeFailure);
    fakePsramAvailable=true;
    PoolDriverConfig heat=pump; heat.capabilities.unit=PoolSetpointUnit::Celsius;
    heat.serial.telemetryProfile=PoolTelemetryProfile::HeatPumpPoly;
    assert(SerialEquipmentValues::define(store,2,"PAC",heat));
    feedback.telemetry.validBlocks=1U<<2;
    feedback.telemetry.readAtMs[2]=6000;
    feedback.telemetry.raw[4]=0xff9c; // -10 C, signed register
    feedback.telemetry.raw[5]=280;
    SerialEquipmentValues::publish(store.values,2,heat,target,feedback,true,6000,6000);
    const auto inlet=SerialEquipmentValues::id(2,EquipmentMetric::WaterIn);
    const auto voltage=SerialEquipmentValues::id(2,EquipmentMetric::Voltage);
    assert(store.values.read(inlet,sample) && sample.quality==ValueQuality::Valid && sample.value.f==-10);
    assert(store.values.read(voltage,sample) && sample.quality==ValueQuality::Invalid);
    ValueId selected[]={speed,inlet};
    const auto bytes=ValueHistory::bytes(24,7,2);
    std::vector<uint64_t> memory((bytes+7)/8);
    ValueHistory history; history.initialize(memory.data(),24,7,selected,2);
    history.clock(6000,1700000000000ULL);
    assert(store.values.read(inlet,sample,&metadata));
    history.observe(inlet,metadata,sample,nullptr); history.tick(7000);
    ValueHistoryRecord record{};
    assert(history.read(inlet,false,0,record) && record.average()==-10);
    assert(!history.read(voltage,false,0,record));
    assert(bytes < ValueHistory::bytes(24,7));
    printf("Sparse serial catalog: pump=%zu host bytes, selected history=%zu bytes; stable high IDs, formulas, expiry, independent policies and signed telemetry OK\n",pumpBytes,bytes);
}

#include "Core/Values/ValueRegistry.h"
#include "Core/DataStore/StartupDataChanges.h"
#include "Core/Values/PulseRuntime.h"
#include "Core/Values/PulseCheckpoint.h"
#include "Modules/PoolHistoryModule/ValueHistory.h"
#include "Modules/IOModule/IODrivers/PcntCounterDriver.h"
#include <assert.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <memory>
#include <thread>
#include <atomic>
#include <stdio.h>

static void near(double actual, double expected, double tolerance=1e-6) {
    if (std::abs(actual-expected)>tolerance) fprintf(stderr,"actual %.12g expected %.12g\n",actual,expected);
    assert(std::abs(actual-expected)<=tolerance);
}
static void pulseTests() {
    PulseRuntime p; p.restore(0,0,0);
    for (uint64_t n=1;n<=60;++n) assert(p.sample(n,uint32_t(n*1000)));
    near(p.rate,60); assert(p.count==60);
    p.sample(60,120000); near(p.rate,0);
    p.sample(61,121000); p.sample(62,122000); near(p.rate,60);
    p.reset(62,1,123000); p.sample(63,124000); assert(p.count==1 && p.generation==1);
    p.restore(1ULL<<40,2,UINT32_MAX-499); p.sample(60,500); near(p.rate,3600); assert(p.count==(1ULL<<40)+60);
    p.restore(0,0,0); p.sample(30,1500); near(p.rate,1200);
    p.sample(75,3750); near(p.rate,1200);
    p.restore(0,0,0); p.sample(1,10000); p.sample(2,20000); near(p.rate,6);
    p.sample(2,25000); near(p.rate,6); p.sample(2,80000); near(p.rate,0);
    p.restore(UINT64_MAX-1,0,0); assert(!p.sample(2,1000)); assert(p.overflow);
    PulseCheckpoint::State state{}; state.count[4]=1ULL<<55; state.generation[4]=7; state.resetToken[4]=2;
    uint8_t bytes[PulseCheckpoint::Size]; PulseCheckpoint::encode(state,bytes);
    p.restore(state.count[4],7,0); p.sample(100,1000);
    PulseCheckpoint::State restored; assert(PulseCheckpoint::decode(bytes,sizeof bytes,restored));
    assert(restored.count[4]==state.count[4]); assert(p.count-restored.count[4]==100);
    assert(restored.resetToken[4]==2); bytes[20]^=1;
    assert(!PulseCheckpoint::decode(bytes,sizeof bytes,restored));
    puts("pulse: regular, irregular, slow, stop, resume, wrap, reset, overflow, checkpoint/power-loss OK");
}
static void driverTests() {
    PcntCounterDriver p("test",1,true,0,1,0); assert(p.begin());
    uint64_t total=0;
    for (int i=0;i<150000;++i) { fakePcnt=30000; ++fakeMs; assert(p.readCount(total)); }
    assert(total==4500000000ULL); assert(fakeStops==150000);
    IODigitalCounterDebugStats stats; p.readDebugStats(stats); assert(stats.irqCalls==total);
    PcntCounterDriver filtered("filtered",2,true,0,1,2000); assert(filtered.begin());
    fakeLevel=HIGH; fakePcnt=3; filtered.readCount(total); assert(total==1);
    fakePcnt=5; filtered.readCount(total); assert(total==1);
    fakeLevel=LOW; ++fakeMs; filtered.readCount(total); fakeMs+=2; filtered.readCount(total);
    fakeLevel=HIGH; fakePcnt=6; filtered.readCount(total); assert(total==2);
    filtered.readDebugStats(stats); assert(stats.ignoredDebounce==4);
    puts("PCNT actual driver: >32-bit accepted/raw counts, folding and existing debounce behavior OK");
}
static void registryTests() {
    ValueRegistry registry;
    assert(registry.begin());
    assert(registry.define(0,{ValueType::UInt64,AggregationMode::Counter,ValueUnit::Pulse}));
    assert(registry.define(1,{ValueType::Float,AggregationMode::Rate,ValueUnit::PulsePerMinute}));
    assert(!registry.define(0,{}));
    assert(registry.defineAffine(2,0,1.0/450,0,AggregationMode::Counter));
    assert(registry.defineAffine(3,1,1.0/450,0,AggregationMode::Rate));
    assert(!registry.defineAffine(4,4,1,0,AggregationMode::Gauge));
    assert(!registry.define(ValueIds::Capacity,{}));
    ValueNumber n; n.u64=154800; registry.write(0,n,1); n.f=1350; registry.write(1,n,1);
    ValueSnapshot result; registry.read(2,result); near(result.value.d,344);
    registry.read(3,result); near(result.value.d,3);
    registry.setAffine(2,2.0/450,0); n.u64=154800; registry.write(0,n,2);
    registry.read(2,result); near(result.value.d,688); assert(result.generation==1);
    std::atomic<bool> done{false};
    std::thread writer([&]{ for(uint64_t i=3;i<100000;++i) { ValueNumber v; v.u64=(1ULL<<50)+i; registry.write(0,v,i); } done=true; });
    while(!done) { registry.read(0,result); if(result.timestampMs>=3) assert(result.value.u64==(1ULL<<50)+result.timestampMs); }
    writer.join();
    puts("registry: typed transforms, capacities, invalid sources, recalibration, concurrent coherent uint64 reads OK");
}
static void historyTests() {
    std::unique_ptr<unsigned char[]> memory(new unsigned char[ValueHistory::bytes(24,7)]);
    ValueHistory h; h.initialize(memory.get(),24,7);
    const uint64_t epoch=20000ULL*24*ValueHistory::HourMs;
    h.clock(0,epoch);
    ValueMetadata gauge{ValueType::Float,AggregationMode::Gauge};
    ValueSnapshot s; s.quality=ValueQuality::Valid; s.sequence=1; s.value.f=10;
    h.observe(0,gauge,s,nullptr); s.timestampMs=1000; s.value.f=20; ++s.sequence; h.observe(0,gauge,s,nullptr);
    h.tick(4000); ValueHistoryRecord record; assert(h.read(0,false,0,record)); near(record.average(),17.5); near(record.minimum,10); near(record.maximum,20);
    gauge.aggregation=AggregationMode::Rate; s.timestampMs=4000; s.value.f=30; h.observe(1,gauge,s,nullptr);
    h.tick(6000); assert(h.read(1,false,0,record)); near(record.average(),30);
    ValueMetadata counter{ValueType::UInt64,AggregationMode::Counter};
    s.timestampMs=6000; s.value.u64=UINT64_C(9007199254740992); h.observe(2,counter,s,nullptr);
    s.timestampMs=7000; s.value.u64+=450; ++s.sequence; h.observe(2,counter,s,nullptr);
    assert(h.read(2,false,0,record)); assert(record.rawDelta==450); near(record.delta,450);
    s.generation=1; s.value.u64=0; s.timestampMs=8000; h.observe(2,counter,s,nullptr);
    s.value.u64=20; s.timestampMs=9000; h.observe(2,counter,s,nullptr);
    assert(h.read(2,false,0,record)); assert(record.rawDelta==470 && record.discontinuities==1);
    ValueMetadata converted{ValueType::Double,AggregationMode::Counter,ValueUnit::Unspecified,2,1.0/450,0};
    ValueSnapshot raw=s; raw.value.u64=1ULL<<55; s.value.d=double(raw.value.u64)/450;
    h.observe(3,converted,s,&raw); s.timestampMs+=1000; raw.value.u64+=450; s.value.d=double(raw.value.u64)/450;
    h.observe(3,converted,s,&raw); assert(h.read(3,false,0,record)); near(record.delta,1);
    // Unequal hourly coverage must be weighted by duration in the day record.
    s={}; s.quality=ValueQuality::Valid; s.sequence=1; s.value.f=10; s.timestampMs=ValueHistory::HourMs-1000;
    h.observe(4,gauge,s,nullptr); s.timestampMs=ValueHistory::HourMs; s.value.f=20; ++s.sequence; h.observe(4,gauge,s,nullptr);
    h.tick(ValueHistory::HourMs+3000); assert(h.read(4,true,0,record)); near(record.average(),17.5);
    assert(h.read(4,false,1,record)); near(record.average(),10);
    h.tick(24*ValueHistory::HourMs); assert(h.read(4,true,1,record)); assert(record.durationMs==901000);
    // Clock rollback breaks interpolation instead of adding a huge duration.
    h.clock(24*ValueHistory::HourMs,epoch); h.tick(24*ValueHistory::HourMs+1000);
    printf("history: weighted gauge/rate, uint64 deltas, converted deltas, reset, hour/day, clock change OK; PSRAM=%zu fallback=%zu bytes\n",ValueHistory::bytes(24,7),ValueHistory::bytes(2,1));
}
static void allocationTests() {
    ValueMetadata metadata{};
    ValueSnapshot snapshot{};
    {
        ValueRegistry registry;
        assert(!registry.read(0, snapshot));
        assert(registry.begin() && registry.storageInPsram());
        const auto allocations = fakeHeapAllocations;
        assert(registry.begin());
        assert(registry.define(0, metadata));
        assert(registry.write(0, ValueNumber{}, 1));
        assert(registry.read(0, snapshot));
        assert(fakeHeapAllocations == allocations);
    }
    fakePsramAvailable = false;
    {
        ValueRegistry registry;
        assert(registry.begin() && !registry.storageInPsram());
    }
    fakeInternalAvailable = false;
    {
        ValueRegistry registry;
        assert(!registry.begin());
        assert(!registry.define(0, metadata));
        assert(!registry.defineAffine(1, 0, 1, 0, AggregationMode::Gauge));
        assert(!registry.setAffine(1, 1, 0));
        assert(!registry.read(0, snapshot));
        assert(!registry.write(0, ValueNumber{}, 1));
        assert(registry.used() == 0);
    }
    fakePsramAvailable = fakeInternalAvailable = true;
    puts("registry allocation: PSRAM, internal fallback, exhaustion, no runtime allocations OK");
}
static void startupNotificationTests() {
    StartupDataChanges changes;
    for (DataKey key = 0; key <= DataKeys::ReservedMax; ++key) {
        assert(changes.mark(key));
        assert(changes.mark(key));
    }
    // A full event queue refuses admission. No dirty key may disappear.
    for (unsigned i = 0; i < 20; ++i) changes.drain(4, [](DataKey) { return false; });
    unsigned delivered[DataKeys::ReservedMax + 1]{};
    for (unsigned pass = 0; pass <= DataKeys::ReservedMax; ++pass) {
        unsigned batch = 0;
        changes.drain(4, [&](DataKey key) { ++batch; ++delivered[key]; return true; });
        assert(batch <= 4);
    }
    for (unsigned count : delivered) assert(count == 1);
    assert(!changes.mark(DataKeys::WifiReady)); // Normal runtime takes over.

    // Update racing with publication must survive both acceptance and refusal.
    for (bool accepted : {false, true}) {
        StartupDataChanges concurrent;
        assert(concurrent.mark(DataKeys::WifiReady));
        concurrent.drain(1, [&](DataKey key) {
            std::thread producer([&] { assert(concurrent.mark(key)); });
            producer.join();
            return accepted;
        });
        unsigned retried = 0;
        concurrent.drain(4, [&](DataKey key) {
            assert(key == DataKeys::WifiReady); ++retried; return true;
        });
        assert(retried == 1);
        assert(!concurrent.mark(DataKeys::WifiReady));
    }
    puts("startup notifications: burst, deduplication, bounded drain, full queue retries, concurrent updates OK");
}
int main() { startupNotificationTests(); allocationTests(); pulseTests(); driverTests(); registryTests(); historyTests(); }

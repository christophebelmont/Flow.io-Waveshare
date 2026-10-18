#include "Core/Values/InternalAnalogSources.h"
#include "Modules/PoolHistoryModule/ValueHistory.h"
#include <cassert>
#include "Core/Values/VolumeUnits.h"
#include <cmath>
#include <cstdio>

int main() {
    using namespace InternalAnalogSources;
    assert(VolumeUnits::millilitresToLitres(2500) == 2.5f);
    assert(VolumeUnits::litresToMillilitres(2.5f) == 2500);
    // A 250 mL dose removes 0.25 L from the remaining volume.
    assert(2.5f - VolumeUnits::millilitresToLitres(250) == 2.25f);
    ValueRegistry registry;
    assert(registry.begin());
    assert(define(registry));
    const auto remaining = tankId(1, TankMetric::RemainingL);
    const auto fill = tankId(1, TankMetric::FillPercent);
    assert(remaining != fill && !ValueIds::isDerived(remaining));
    assert(!contains(VALUE_INVALID) && !contains(ValueIds::Analog));
    assert(tankId(16, TankMetric::FillPercent) == VALUE_INVALID);
    ValueSnapshot snapshot;
    ValueMetadata meta;
    assert(registry.read(fill, snapshot, &meta));
    assert(snapshot.quality == ValueQuality::Unknown);
    assert(!fresh(snapshot, 0));
    assert(meta.aggregation == AggregationMode::Gauge);
    assert(meta.displayUnit[0] == '%');

    publishTank(registry, 1, 2.5f, 10.0f, true, 1000);
    assert(registry.read(remaining, snapshot, &meta));
    assert(snapshot.value.f == 2.5f && fresh(snapshot, 1000));
    assert(meta.displayUnit[0] == 'L');
    assert(registry.read(fill, snapshot));
    assert(snapshot.value.f == 25);
    assert(fresh(snapshot, 6000) && !fresh(snapshot, 6001));
    assert(!fresh(snapshot, 999));
    const auto sequence = snapshot.sequence;
    // An idle producer refreshes its timestamp without altering the value.
    publishTank(registry, 1, 2.5f, 10.0f, true, 2000);
    assert(registry.read(fill, snapshot));
    assert(snapshot.sequence > sequence && fresh(snapshot, 7000));

    publishTank(registry, 1, 0, 10.0f, true, 3000);
    assert(registry.read(fill, snapshot));
    assert(snapshot.value.f == 0 && fresh(snapshot, 3000));
    // Refill and capacity change are absolute gauges, not counters.
    publishTank(registry, 1, 10.0f, 10.0f, true, 4000);
    assert(registry.read(fill, snapshot) && snapshot.value.f == 100);
    publishTank(registry, 1, 10.0f, 20.0f, true, 5000);
    assert(registry.read(fill, snapshot) && snapshot.value.f == 50);

    const float invalid[][2] = {{0,0}, {1,-1}, {NAN,100}, {100,INFINITY}, {-1,100}, {101,100}};
    uint64_t now = 6000;
    for (const auto& input : invalid) {
        publishTank(registry, 1, input[0], input[1], true, now);
        assert(registry.read(fill, snapshot) && !fresh(snapshot, now));
        assert(registry.read(remaining, snapshot) && snapshot.quality == ValueQuality::Invalid);
        ++now;
    }
    publishTank(registry, 1, 0.5f, 1.0f, false, ++now);
    assert(registry.read(fill, snapshot) && snapshot.quality == ValueQuality::Invalid);
    publishTank(registry, 1, 0.5f, 1.0f, true, ++now);
    assert(registry.read(fill, snapshot) && fresh(snapshot, now));
    // Slots never alias; unsupported/unpublished devices remain unknown.
    assert(registry.read(tankId(2, TankMetric::FillPercent), snapshot));
    assert(snapshot.quality == ValueQuality::Unknown);

    // The analog alias feeds the existing expression engine and propagates invalidity.
    assert(registry.define(ValueIds::Analog + 12, {}));
    ValueExpression::Program program;
    assert(ValueExpression::compile("a12 * 2", program));
    double parameters[4] = {1,0,0,0};
    assert(registry.defineProgram(ValueIds::Derived, program, parameters, AggregationMode::Gauge));
    assert(registry.read(fill, snapshot));
    assert(registry.write(ValueIds::Analog + 12, snapshot.value, now, snapshot.quality));
    assert(registry.read(ValueIds::Derived, snapshot) && snapshot.value.d == 100);
    assert(registry.write(ValueIds::Analog + 12, {}, ++now, ValueQuality::Invalid));
    assert(registry.read(ValueIds::Derived, snapshot) && snapshot.quality == ValueQuality::Invalid);
    puts("Tank sources: identities, units, empty/refill, capacity, invalidity, freshness, isolation, expressions OK");
}

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "Core/Values/PulseRateOnly.h"
#include "Core/Values/PulseFlowUnits.h"

static bool near(double got, double expected, double eps = 0.0001) {
    return std::fabs(got - expected) <= eps;
}

int main() {
    // Two simultaneous production inputs, counted by PCNT, sampled every 20ms.
    PulseRateOnly a{}, b{};
    uint64_t aRaw = 0, bRaw = 0;
    unsigned aUpdates = 0, bUpdates = 0;
    for (uint32_t t = 0; t <= 10000; t += 20) {
        // 1 pulse per 100ms -> nominal 600 pulses/min (after warmup).
        if (t > 0 && t % 100 == 0) ++aRaw;
        // 1 pulse per 200ms -> nominal 300 pulses/min.
        if (t > 0 && t % 200 == 0) ++bRaw;
        assert(a.sample(aRaw, t) && b.sample(bRaw, t));
        if (a.refreshed) ++aUpdates;
        if (b.refreshed) ++bUpdates;
    }
    assert(aUpdates == 11 && bUpdates == 11);
    assert(near(a.pulsesPerMinute, 600));
    assert(near(b.pulsesPerMinute, 300));
    // C0=20 pulses/liter: 600 pulses/min = 30 L/min.
    assert(near(pulseFlowConvert(a.pulsesPerMinute, 20, IO_FLOW_LITERS_PER_MINUTE), 30));
    assert(near(pulseFlowConvert(a.pulsesPerMinute, 20, IO_FLOW_CUBIC_METERS_PER_MINUTE), 0.03));
    assert(near(pulseFlowConvert(a.pulsesPerMinute, 20, IO_FLOW_LITERS_PER_HOUR), 1800));
    assert(near(pulseFlowConvert(a.pulsesPerMinute, 20, IO_FLOW_CUBIC_METERS_PER_HOUR), 1.8));
    assert(near(pulseFlowConvert(230, 23, IO_FLOW_LITERS_PER_MINUTE), 10));
    assert(near(pulseFlowConvert(230, 23, IO_FLOW_CUBIC_METERS_PER_HOUR), 0.6));
    assert(!pulseFlowUnitValid(4));
    assert(std::isnan(pulseFlowConvert(600, -1, IO_FLOW_LITERS_PER_MINUTE)));
    assert(std::isnan(pulseFlowConvert(NAN, 1, IO_FLOW_LITERS_PER_MINUTE)));
    assert(std::isnan(pulseFlowConvert(600, 1, 99)));
    assert(near(pulseFlowConvert(0, 20, IO_FLOW_LITERS_PER_HOUR), 0));
    assert(std::isnan(pulseFlowConvert(100, 0, IO_FLOW_LITERS_PER_MINUTE)));

    // Stop incoming pulses: rate returns to zero, without a cumulative total.
    for (uint32_t t = 10020; t <= 12000; t += 20) a.sample(aRaw, t);
    assert(near(a.pulsesPerMinute, 0));
    // Hardware restart/rebase does not produce a negative or absurd rate.
    a.sample(0, 12020);
    assert(near(a.pulsesPerMinute, 0));
    // Timer wrap keeps the one-second measurement rule.
    PulseRateOnly rollover{};
    rollover.sample(0, UINT32_MAX - 499U);
    rollover.sample(10, 501U); // elapsed ~1001ms modulo uint32
    assert(rollover.refreshed && rollover.valid);
    assert(rollover.pulsesPerMinute > 500 && rollover.pulsesPerMinute < 700);
    std::puts("PulseRateOnly: two channels, 1-second updates, four units, idle, reboot, wrap OK");
}

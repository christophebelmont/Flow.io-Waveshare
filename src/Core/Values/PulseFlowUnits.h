#pragma once
#include <stdint.h>
#include <math.h>

// Stable persisted numeric options. Existing values 0 and 1 are preserved.
enum IOFlowUnit : uint8_t {
    IO_FLOW_LITERS_PER_MINUTE = 0,
    IO_FLOW_CUBIC_METERS_PER_MINUTE = 1,
    IO_FLOW_LITERS_PER_HOUR = 2,
    IO_FLOW_CUBIC_METERS_PER_HOUR = 3
};

inline bool pulseFlowUnitValid(uint8_t unit) {
    return unit <= IO_FLOW_CUBIC_METERS_PER_HOUR;
}

inline const char* pulseFlowUnitText(uint8_t unit) {
    switch (unit) {
        case IO_FLOW_CUBIC_METERS_PER_HOUR: return "m3/h";
        case IO_FLOW_LITERS_PER_HOUR: return "l/h";
        case IO_FLOW_CUBIC_METERS_PER_MINUTE: return "m3/min";
        case IO_FLOW_LITERS_PER_MINUTE: return "l/min";
        default: return "";
    }
}

// K (C0) is pulses per liter in mode 2. Divide pulse frequency
// (pulses/min) by K to obtain liters/min, then convert units.
inline double pulseFlowConvert(double pulsesPerMinute, double pulsesPerLiter, uint8_t unit) {
    if (!pulseFlowUnitValid(unit) || !isfinite(pulsesPerMinute) ||
        !isfinite(pulsesPerLiter) || pulsesPerMinute < 0.0 || pulsesPerLiter <= 0.0)
        return NAN;
    const double litersPerMinute = pulsesPerMinute / pulsesPerLiter;
    switch (unit) {
        case IO_FLOW_CUBIC_METERS_PER_HOUR: return litersPerMinute * 0.06;
        case IO_FLOW_LITERS_PER_HOUR: return litersPerMinute * 60.0;
        case IO_FLOW_CUBIC_METERS_PER_MINUTE: return litersPerMinute / 1000.0;
        case IO_FLOW_LITERS_PER_MINUTE: return litersPerMinute;
        default: return NAN;
    }
}

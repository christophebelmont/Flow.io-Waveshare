#pragma once
#include <ArduinoJson.h>
#include "Core/Services/IPoolHistory.h"
#include "Modules/PoolHistoryModule/ValueHistory.h"

namespace HistoryJson {
template<class JsonMember>
inline void number(JsonMember out, bool valid, double value) {
    if (valid && isfinite(value)) out.set(value);
    else out.set(nullptr);
}
inline void metric(JsonObject out, const PoolHistoryMetricSummary& value) {
    out["samples"] = value.sampleCount;
    number(out["average"], value.valid, value.average);
    number(out["min"], value.valid, value.minimum);
    number(out["max"], value.valid, value.maximum);
    number(out["first"], value.valid, value.first);
    number(out["last"], value.valid, value.last);
}
inline void activity(JsonObject out, const PoolHistoryActivitySummary& value) {
    number(out["seconds"], value.valid, value.runningSec);
    out["observed_seconds"] = value.observedSec;
    JsonArray periods = out.createNestedArray("periods");
    for (const auto& period : value.periods) {
        JsonObject item = periods.createNestedObject();
        number(item["seconds"], period.valid, period.runningSec);
        item["observed_seconds"] = period.observedSec;
    }
}
inline void day(JsonObject out, const PoolHistoryDaySummary& value) {
    out["date"] = value.localDate;
    out["valid"] = value.valid;
    out["complete"] = value.complete;
    out["from"] = value.observedFromUtc;
    out["until"] = value.observedUntilUtc;
    activity(out.createNestedObject("filtration"), value.filtration);
    activity(out.createNestedObject("heating"), value.heating);
    const PoolHistoryMetricSummary* metrics[] = {
        &value.waterTemperature, &value.airTemperature, &value.ph, &value.orp,
        &value.daytimeWaterTemperature, &value.nighttimeWaterTemperature,
        &value.phSetpoint, &value.orpSetpoint, &value.heaterSetpoint
    };
    JsonArray items = out.createNestedArray("metrics");
    for (const auto* item : metrics) metric(items.createNestedObject(), *item);
    number(out["night_delta"], value.dayToNightTemperatureVariationValid,
           value.dayToNightTemperatureVariationC);
    number(out["refill_litres"], value.refillVolumeValid, value.refillVolumeLitres);
    number(out["refill_events"], value.refillEventsValid, value.refillEventCount);
}
inline void record(JsonObject out, const ValueHistoryRecord& value, bool daily) {
    out["start_utc"] = value.period * (daily ? 86400ULL : 3600ULL);
    out["coverage_ms"] = value.durationMs;
    number(out["average"], value.valid, value.average());
    number(out["min"], value.valid, value.minimum);
    number(out["max"], value.valid, value.maximum);
    number(out["delta"], value.valid, value.delta);
    // Decimal strings preserve all 64 bits in browser clients.
    char raw[24];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)value.rawDelta);
    out["raw_delta"] = raw;
    out["discontinuities"] = value.discontinuities;
    out["boundary_uncertain"] = value.boundaryUncertain;
}
}

#include "Core/Values/DataValueView.h"
#include "Core/Values/PoolDeviceValues.h"
#include "Core/Values/InternalAnalogSources.h"
#include "Core/Values/ValueFormat.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    ValueRegistry values;
    assert(values.begin());
    assert(InternalAnalogSources::define(values));
    assert(PoolDeviceValues::define(values));
    const ValueId fill = dataValueReference("pd2.tank.fill_percent", strlen("pd2.tank.fill_percent"));
    const ValueId remaining = dataValueReference("pd2.tank.remaining_l", strlen("pd2.tank.remaining_l"));
    assert(fill == 117 && remaining == 116);
    assert(dataValueReference("pd16.tank.fill_percent", 22) == VALUE_INVALID);
    assert(!ValueIds::isDerived(fill));
    assert(isValidRuntimeUiId(kDataRuntimeUiBase + fill));
    assert(!isValidRuntimeUiId(kDataRuntimeUiBase + ValueIds::Capacity));
    ValueExpression::Program program{};
    assert(ValueExpression::compile("pd2.tank.remaining_l + pd2.tank.fill_percent", program));
    const double parameters[4] = {1,0,0,0};
    values.setHistory(ValueIds::Derived, false);
    assert(values.defineProgram(ValueIds::Derived, program, parameters, AggregationMode::Gauge, "", 2));
    values.setName(fill, "Bidon chlore");
    ValueSnapshot sample{}; ValueMetadata metadata{};
    DataValuePresentation presentation{};
    assert(!presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    assert(!readAvailableDataValue(values, fill, sample, metadata));
    // No analog input exists. Equipment data flows directly to views and formulas.
    InternalAnalogSources::publishTank(values, 2, 0.5f, 1.0f, true, 1000);
    assert(presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    assert(presentation.available && presentation.wireType == RuntimeUiWireType::Float64);
    assert(readAvailableDataValue(values, fill, sample, metadata));
    assert(presentation.f64Value == 50 && strcmp(metadata.displayUnit, "%") == 0);
    assert(strcmp(dataValueLabel(fill, metadata), "Bidon chlore") == 0);
    assert(values.read(ValueIds::Derived, sample, &metadata));
    assert(sample.value.d == 50.5 && !metadata.history);
    assert(runtimeDataValueId(makeRuntimeUiId(ModuleId::Io, 80)) == VALUE_INVALID);
    assert(presentDataValue(values, kDataRuntimeUiBase + ValueIds::Derived, presentation, metadata));
    assert(presentation.f64Value == 50.5);
    values.expire(6000);
    assert(presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    values.expire(6001);
    assert(!readAvailableDataValue(values, fill, sample, metadata));
    assert(!presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    assert(values.read(ValueIds::Derived, sample) && sample.quality == ValueQuality::Invalid);
    // Reading or formatting does not refresh the measurement.
    assert(values.read(fill, sample) && sample.timestampMs == 1000 && sample.qualityChangedAtMs == 6001);
    InternalAnalogSources::publishTank(values, 2, 1.0f, 1.0f, true, 7000);
    assert(presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata) && presentation.f64Value == 100);
    assert(values.read(ValueIds::Derived, sample) && sample.quality == ValueQuality::Valid);
    assert(strcmp(dataValueDescriptor(fill)->haSuffix, "data_117") == 0);
    values.setName(fill, "Nouveau nom");
    assert(strcmp(dataValueDescriptor(fill)->haSuffix, "data_117") == 0);

    // User-facing usage is independent of acquisition, history and internal conversion.
    values.setUsable(fill, false);
    assert(!values.usable(fill));
    assert(!presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    assert(values.read(fill, sample, &metadata));
    assert(sample.quality == ValueQuality::Valid && metadata.history && !metadata.usable);
    assert(readAvailableDataValue(values, fill, sample, metadata)); // HA/history remain independent of usage.
    assert(values.read(ValueIds::Derived, sample) && sample.quality == ValueQuality::Invalid);
    assert(values.defineAffine(ValueIds::Analog, fill, 1, 0, AggregationMode::Gauge));
    assert(values.read(ValueIds::Analog, sample) && sample.quality == ValueQuality::Valid);
    values.setUsable(fill, true);
    assert(presentDataValue(values, kDataRuntimeUiBase + fill, presentation, metadata));
    assert(values.read(ValueIds::Derived, sample) && sample.quality == ValueQuality::Valid);
    values.setUsable(ValueIds::Analog + 1, false);
    ValueMetadata hidden{};
    assert(values.define(ValueIds::Analog + 1, hidden));
    assert(values.read(ValueIds::Analog + 1, sample, &metadata) && !metadata.usable);

    // Digital states have their own typed identity; false is a valid measurement.
    ValueMetadata state{}; state.type=ValueType::Bool; state.maxAgeMs=5000;
    const auto stateId = ValueIds::DigitalState+4;
    assert(values.define(stateId, state));
    assert(values.write(stateId, {}, 8000));
    assert(readAvailableDataValue(values, stateId, sample, metadata));
    assert(presentDataValue(values, kDataRuntimeUiBase+stateId, presentation, metadata));
    assert(presentation.wireType==RuntimeUiWireType::Bool && !presentation.boolValue);
    char stateJson[160];
    assert(formatValueSnapshot(stateJson,sizeof(stateJson),stateId,sample,metadata));
    assert(strstr(stateJson,"\"value\":false"));
    assert(ValueExpression::compile("i04.state * 2", program));
    assert(values.defineProgram(ValueIds::Derived+2,program,parameters,AggregationMode::Gauge,"",0));
    assert(values.read(ValueIds::Derived+2,sample) && sample.quality==ValueQuality::Valid && sample.value.d==0);
    // Daily equipment uptime is seconds, available without an IO input or tank.
    const auto uptime = PoolDeviceValues::uptimeId(15);
    assert(uptime == 159 && !InternalAnalogSources::contains(uptime));
    assert(dataValueReference("pd15.uptime_day_s", strlen("pd15.uptime_day_s")) == uptime);
    assert(!presentDataValue(values, kDataRuntimeUiBase + uptime, presentation, metadata));
    PoolDeviceValues::publishUptime(values, 15, 1234567, true, 8000);
    assert(presentDataValue(values, kDataRuntimeUiBase + uptime, presentation, metadata));
    assert(strcmp(presentation.stringValue, "1234") == 0 && strcmp(metadata.displayUnit, "s") == 0);
    ValueExpression::Program uptimeFormula{};
    assert(ValueExpression::compile("pd15.uptime_day_s / 60", uptimeFormula));
    assert(values.defineProgram(ValueIds::Derived + 1, uptimeFormula, parameters, AggregationMode::Gauge));
    assert(values.read(ValueIds::Derived + 1, sample) && sample.quality == ValueQuality::Valid);
    values.expire(13001);
    assert(values.read(uptime, sample) && sample.quality == ValueQuality::Invalid);
    PoolDeviceValues::publishUptime(values, 15, 0, true, 14000); // New day/reset.
    assert(presentDataValue(values, kDataRuntimeUiBase + uptime, presentation, metadata));
    assert(strcmp(presentation.stringValue, "0") == 0);
    assert(values.read(ValueIds::Derived + 1, sample) && sample.value.d == 0);
    PoolDeviceValues::publishUptime(values, 15, 1000, false, 15000);
    assert(!presentDataValue(values, kDataRuntimeUiBase + uptime, presentation, metadata));

    // Counter values never pass through a floating point representation for display or MQTT.
    ValueMetadata count{}; count.type = ValueType::UInt64;
    assert(values.define(ValueIds::Digital, count));
    ValueNumber large; large.u64 = UINT64_MAX;
    assert(values.write(ValueIds::Digital, large, 7000));
    assert(presentDataValue(values, kDataRuntimeUiBase + ValueIds::Digital, presentation, metadata));
    assert(presentation.wireType == RuntimeUiWireType::String);
    assert(strcmp(presentation.stringValue, "18446744073709551615") == 0);
    assert(values.read(ValueIds::Digital, sample, &metadata));
    char json[160];
    assert(formatValueSnapshot(json, sizeof(json), ValueIds::Digital, sample, metadata));
    assert(strstr(json, "18446744073709551615"));
    ValueMetadata digital{}; digital.type = ValueType::Bool;
    assert(values.define(ValueIds::Digital + 1, digital));
    assert(values.write(ValueIds::Digital + 1, {}, 7000));
    assert(!presentDataValue(values, kDataRuntimeUiBase + ValueIds::Digital + 1, presentation, metadata));
    for (size_t i = 0; i < kDataValueCount; ++i) {
        const auto& d = kDataValues[i];
        assert(runtimeDataValueId(kDataRuntimeUiBase + d.id) == d.id);
        assert(dataValueReference(d.reference, strlen(d.reference)) == d.id);
        for (size_t j = 0; j < i; ++j) assert(strcmp(d.haSuffix, kDataValues[j].haSuffix));
    }
    puts("DataStore catalog: direct views/formulas, canonical IDs, expiry/recovery, policy, exact counters and stable identity OK");
}

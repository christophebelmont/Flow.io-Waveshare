#include <cassert>
#include <stdint.h>
#include "Modules/IOModule/IOBus/OneWireBus.h"
#include "Modules/IOModule/IODrivers/Ds18b20Driver.h"

uint32_t testNowMs = 0;

namespace {
struct ProbeBus {
    unsigned requests = 0;
    unsigned reads = 0;
    float temperature = 25.0f;
};

ProbeBus probeBuses[2];
constexpr uint32_t commandDurationMs = 3U;
using Step = Ds18b20Driver::PollStep;

Step pollAt(Ds18b20Driver& driver, uint32_t nowMs)
{
    testNowMs = nowMs;
    return driver.poll(nowMs);
}
}

void DallasTemperature::requestTemperatures()
{
    assert(!waitForConversion_);
    ++probeBuses[bus_->pin()].requests;
    testNowMs += commandDurationMs;
}

float DallasTemperature::getTempC(const uint8_t*) const
{
    auto& probe = probeBuses[bus_->pin()];
    ++probe.reads;
    return probe.temperature;
}

int main()
{
    const uint8_t address[8] = {};
    const Ds18b20DriverConfig config{};
    assert(config.periodMs() == 10000U);
    assert(Ds18b20DriverConfig{0U}.periodMs() == 750U);

    OneWireBus waterBus(0);
    OneWireBus airBus(1);
    Ds18b20Driver water("water", &waterBus, address, config);
    Ds18b20Driver air("air", &airBus, address, config);
    assert(water.begin());
    assert(air.begin());
    water.scheduleFirstConversion(1000U, 0U);
    air.scheduleFirstConversion(1000U, config.periodMs() / 2U);

    IOAnalogSample sample{};
    assert(!water.readSample(0, sample));
    assert(pollAt(water, 1000U) == Step::Convert);
    assert(probeBuses[0].requests == 1 && probeBuses[0].reads == 0);
    assert(pollAt(water, 1752U) == Step::Idle);
    assert(pollAt(water, 1753U) == Step::Read);
    assert(pollAt(water, 1754U) == Step::Idle);
    assert(probeBuses[0].requests == 1 && probeBuses[0].reads == 1);
    assert(water.readSample(0, sample) && sample.value == 25.0f);
    assert(pollAt(air, 5999U) == Step::Idle);
    assert(pollAt(air, 6000U) == Step::Convert);
    assert(pollAt(air, 6753U) == Step::Read);
    assert(pollAt(water, 10999U) == Step::Idle);

    // A missed deadline preserves each probe's original phase.
    assert(pollAt(water, 13550U) == Step::Convert);
    assert(pollAt(water, 14303U) == Step::Read);
    assert(pollAt(air, 16000U) == Step::Convert);
    assert(pollAt(air, 16753U) == Step::Read);
    assert(pollAt(water, 20999U) == Step::Idle);
    assert(pollAt(water, 21000U) == Step::Convert);
    probeBuses[0].temperature = DEVICE_DISCONNECTED_C;
    assert(pollAt(water, 21753U) == Step::Read);
    assert(!water.readSample(0, sample));
    assert(pollAt(water, 31000U) == Step::Convert);
    probeBuses[0].temperature = 24.5f;
    assert(pollAt(water, 31753U) == Step::Read);
    assert(water.readSample(0, sample) && sample.value == 24.5f);

    // Conversion wait and the next periodic deadline survive millis() rollover.
    const uint32_t start = UINT32_MAX - 500U;
    air.scheduleFirstConversion(start, 0U);
    assert(pollAt(air, start) == Step::Convert);
    assert(pollAt(air, start + 752U) == Step::Idle);
    assert(pollAt(air, start + 753U) == Step::Read);
    assert(pollAt(air, start + 9999U) == Step::Idle);
    assert(pollAt(air, start + 10000U) == Step::Convert);
}

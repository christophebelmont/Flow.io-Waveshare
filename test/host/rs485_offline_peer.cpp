#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Modules/IOModule/IOScheduler/Rs485TransactionScheduler.h"
#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"
#include "Modules/PoolDeviceModule/Drivers/PoolSerialProfiles.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"

uint32_t testNowMs = 0;

class SharedBus : public IRs485Transport {
public:
    bool active = false, available = false;
    uint32_t sentAt = 0;
    uint16_t pumpRpm = 0;
    unsigned pacTransmissions = 0;
    std::vector<uint8_t> reply;
    bool ready() const override { return true; }
    bool configureLine(const SerialLineProfile&) override { return !active; }
    bool quiet(uint32_t) const override { return !active && !available; }
    bool receivingFrame() const override { return false; }
    bool frameAvailable() const override { return available; }
    void tick(uint32_t) override {
        if (active && !reply.empty() && uint32_t(testNowMs - sentAt) >= 230) available = true;
    }
    bool startTransmit(const uint8_t* frame, size_t length, uint32_t) override {
        assert(!active && length == 8);
        active = true; available = false; sentAt = testNowMs; reply.clear();
        if (frame[0] == 17) { ++pacTransmissions; return true; } // Silent PAC.
        assert(frame[0] == 0xAA);
        if (frame[1] == 0xD0) {
            const uint16_t raw = uint16_t(frame[4]) << 8 | frame[5];
            pumpRpm = raw == 1 ? 0 : raw;
            reply = {0xAA, 0xD0, 0x0B, 0xB9, 0, 2, 0};
        } else {
            assert(frame[1] == 0xC3);
            reply = {0xAA, 0xC3, 0, 0, uint8_t(pumpRpm != 0), uint8_t(pumpRpm >> 8), uint8_t(pumpRpm)};
        }
        const auto crc = ModbusRtuCodec::crc16(reply.data(), reply.size());
        reply.push_back(uint8_t(crc)); reply.push_back(uint8_t(crc >> 8));
        return true;
    }
    bool takeFrame(uint8_t* out, size_t capacity, size_t& length) override {
        assert(available && capacity >= reply.size());
        length = reply.size(); std::memcpy(out, reply.data(), length);
        active = available = false; return true;
    }
    void abort() override { active = available = false; reply.clear(); }
};

uint32_t exercise(uint32_t commandAt, bool responsiveTransport = true) {
    testNowMs = 0;
    SharedBus bus;
    Rs485TransactionScheduler scheduler(bus);
    assert(scheduler.begin());
    PoolDriverConfig pumpConfig, pacConfig;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, pumpConfig));
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, pacConfig));
    SerialDeviceDriver pump, pac;
    assert(pump.begin(pumpConfig, nullptr, &scheduler.service(), 1));
    assert(pac.begin(pacConfig, nullptr, &scheduler.service(), 8));
    bool started = false, confirmed = false;
    uint32_t requestedAt = 0, confirmedAt = 0;
    for (testNowMs = 10; testNowMs < 70000; testNowMs += 10) {
        // UART service now preempts web handlers, while application tasks may
        // still suffer the ~800 ms gaps observed in the incident.
        if (responsiveTransport) scheduler.tick(testNowMs, testNowMs * 1000);
        if (testNowMs % 1100 < 800) continue;
        if (!started && testNowMs >= commandAt) {
            pump.applyTarget({true, 2200}, 1);
            started = true; requestedAt = testNowMs;
        }
        if (!responsiveTransport) scheduler.tick(testNowMs, testNowMs * 1000);
        pac.tick(testNowMs, true);
        pump.tick(testNowMs, true);
        const auto state = pump.readState();
        if (started) {
            // PoolDevice would force an automatic stop on stale feedback.
            if (!responsiveTransport && state.quality == PoolFeedbackQuality::Stale) return UINT32_MAX;
            assert(state.quality != PoolFeedbackQuality::Stale);
            if (state.observedValid && state.observed.running && state.observed.setpoint == 2200) {
                if (!confirmed) confirmedAt = testNowMs;
                confirmed = true;
            }
        }
    }
    assert(confirmed && bus.pumpRpm == 2200);
    assert(pac.readState().error == MODBUS_RESULT_TIMEOUT);
    assert(bus.pacTransmissions > 4); // Recovery attempts continue.
    return confirmedAt - requestedAt;
}

int main() {
    assert(exercise(37250, false) == UINT32_MAX); // Reproduce starvation before priority isolation.
    uint32_t maximumDelay = 0;
    for (uint32_t commandAt = 35000; commandAt <= 45000; commandAt += 250) {
        const uint32_t delay = exercise(commandAt);
        if (delay > maximumDelay) maximumDelay = delay;
    }
    std::printf("Offline PAC isolation passed at 41 command timings; max confirmation delay %u ms\n", maximumDelay);
}

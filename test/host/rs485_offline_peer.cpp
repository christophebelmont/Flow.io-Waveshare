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
    unsigned pacTransmissions = 0, pacReplies = 0;
    uint32_t disconnectPacAt = 0;
    uint16_t pacControl = 0, pacTemperature = 250;
    unsigned pumpStopsAfterStart = 0;
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
        if (frame[0] == 17) {
            ++pacTransmissions;
            if (testNowMs >= disconnectPacAt) return true;
            ++pacReplies;
            const uint16_t address = uint16_t(frame[2]) << 8 | frame[3];
            const uint16_t data = uint16_t(frame[4]) << 8 | frame[5];
            if (frame[1] == 6) {
                if (address == 1000) pacControl = data;
                else if (address == 1001) pacTemperature = data;
                reply.assign(frame, frame + 6);
            } else {
                assert(frame[1] == 3);
                reply = {17, 3, uint8_t(data * 2)};
                for (unsigned i = 0; i < data; ++i) {
                    const auto reg = address + i;
                    const uint16_t value = reg == 58 ? 320 : reg == 500 ? 8 :
                        reg == 1000 ? pacControl : reg == 1001 ? pacTemperature : 0;
                    reply.push_back(uint8_t(value >> 8));
                    reply.push_back(uint8_t(value));
                }
            }
        } else if (frame[1] == 0xD0) {
            assert(frame[0] == 0xAA);
            const uint16_t raw = uint16_t(frame[4]) << 8 | frame[5];
            if (raw == 1 && pumpRpm) ++pumpStopsAfterStart;
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

uint32_t exercise(uint32_t commandAt, bool responsiveTransport = true, uint32_t disconnectPacAt = 0) {
    testNowMs = 0;
    SharedBus bus;
    bus.disconnectPacAt = disconnectPacAt;
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
            if (responsiveTransport) assert(state.quality != PoolFeedbackQuality::Stale);
            if (state.observedValid && state.observed.running && state.observed.setpoint == 2200) {
                if (!confirmed) confirmedAt = testNowMs;
                confirmed = true;
            }
        }
    }
    assert(confirmed && bus.pumpRpm == 2200 && bus.pumpStopsAfterStart == 0);
    if (disconnectPacAt) assert(bus.pacReplies > 0);
    assert(pac.readState().error == MODBUS_RESULT_TIMEOUT);
    assert(bus.pacTransmissions > 4); // Recovery attempts continue.
    return confirmedAt - requestedAt;
}

int main() {
    assert(exercise(37250, false) < 15000); // Long application stalls still allow a healthy pump to run.
    assert(exercise(1000, true, 10000) < 9000); // Unplug a previously responsive PAC while filtration runs.
    uint32_t maximumDelay = 0;
    for (uint32_t commandAt = 35000; commandAt <= 45000; commandAt += 250) {
        const uint32_t delay = exercise(commandAt);
        if (delay > maximumDelay) maximumDelay = delay;
    }
    std::printf("Offline PAC isolation passed at 41 command timings; max confirmation delay %u ms\n", maximumDelay);
}

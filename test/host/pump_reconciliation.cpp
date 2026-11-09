#include <cassert>
#include <cstdio>
#include <vector>
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/PoolDeviceModule/Drivers/PoolSerialProfiles.h"

uint32_t testNowMs = 0;

// The pump accepts RPM immediately, but returns its startup RPM for 60 seconds.
struct Pump {
    struct Write { uint32_t at; uint16_t value; };
    ModbusMasterService service{};
    ModbusRequest pending{};
    bool queued = false, running = false, rejectStart = false;
    bool loseNextAck = false, offline = false, holdWrongSpeed = false;
    uint16_t requested = 2900;
    uint32_t startedAt = 0;
    uint16_t nextId = 0;
    std::vector<Write> writes;
    Pump() {
        service.ctx = this;
        service.submit = [](void* ctx, const ModbusRequest* request, uint16_t* id) {
            auto& p = *static_cast<Pump*>(ctx);
            assert(!p.queued);
            p.pending = *request; p.queued = true; *id = ++p.nextId;
            if (request->operation == RegisterOperation::WriteSingle) {
                p.writes.push_back({testNowMs, request->values[0]});
                if (!p.offline) {
                    if (request->values[0] == 1) p.running = false;
                    else {
                        if (!p.running) p.startedAt = testNowMs;
                        p.running = !p.rejectStart;
                        p.requested = request->values[0];
                    }
                }
            }
            return MODBUS_RESULT_OK;
        };
        service.poll = [](void* ctx, uint16_t, ModbusResponse* response) {
            auto& p = *static_cast<Pump*>(ctx);
            assert(p.queued); p.queued = false;
            *response = {};
            const bool lostAck = p.loseNextAck && p.pending.operation == RegisterOperation::WriteSingle;
            if (lostAck) p.loseNextAck = false;
            response->result = p.offline || lostAck ? MODBUS_RESULT_TIMEOUT : MODBUS_RESULT_OK;
            response->registerCount = 1;
            const uint16_t rpm = !p.running ? 0 :
                uint32_t(testNowMs - p.startedAt) < 60000 || p.holdWrongSpeed ? 2900 : p.requested;
            response->values[0] = rpm;
            response->aquaGem = {true, 0, uint8_t(p.running), rpm};
            return MODBUS_RESULT_OK;
        };
        service.cancelOwner = [](void* ctx, uint8_t) { static_cast<Pump*>(ctx)->queued = false; };
    }
};
struct Scenario {
    Pump pump;
    SerialDeviceDriver driver;
    uint32_t now;
    Scenario(uint32_t start = 100) : now(start) {
        testNowMs = now;
        PoolDriverConfig config;
        assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
        assert(driver.begin(config, nullptr, &pump.service, 1));
    }
    void target(bool running, float rpm, uint32_t revision) {
        testNowMs = now; driver.applyTarget({running, rpm}, revision);
    }
    void advance(uint32_t duration) {
        for (uint32_t i = 0; i < duration; ++i) {
            testNowMs = now++; driver.tick(testNowMs, true);
        }
    }
    void confirmed(float rpm) const {
        const auto& state = driver.readState();
        if (state.phase != PoolCommandPhase::Applied)
            fprintf(stderr, "not confirmed at %u: phase=%u writes=%zu run=%u rpm=%.0f appliedValid=%u\n",
                now, unsigned(state.phase), pump.writes.size(), state.observed.running,
                double(state.observed.setpoint), state.appliedValid);
        assert(state.observedValid && state.observed.running && state.observed.setpoint == rpm);
        assert(state.phase == PoolCommandPhase::Applied);
    }
};

void startupAndNewTarget(uint32_t start) {
    Scenario s(start);
    s.target(true, 1800, 1); s.advance(30000);
    assert(s.pump.writes.size() == 1);
    assert(s.driver.readState().observed.running);
    assert(s.driver.readState().phase == PoolCommandPhase::Confirming);
    s.target(true, 2100, 2); s.advance(29000);
    assert(s.pump.writes.size() == 2);
    assert(s.driver.readState().observed.setpoint == 2900);
    s.advance(3000); s.confirmed(2100);
    assert(s.pump.writes.size() == 2); // No write storm or restarted grace period.
}
void lostAcknowledgement() {
    Scenario s;
    s.pump.loseNextAck = true;
    s.target(true, 1800, 1); s.advance(59000);
    assert(s.pump.writes.size() == 1);
    assert(s.driver.readState().phase == PoolCommandPhase::VerifyingWrite);
    s.advance(4000); s.confirmed(1800);
    assert(s.pump.writes.size() == 1);
}
void physicalRestartAndDrift() {
    Scenario s;
    s.target(true, 1800, 1); s.advance(62000); s.confirmed(1800);
    s.pump.running = false; // Watchdog/power loss, still answering status reads.
    s.advance(2000);
    assert(s.pump.writes.size() == 2 && s.pump.writes.back().value == 1800);
    s.advance(57000);
    assert(s.pump.writes.size() == 2);
    s.advance(5000); s.confirmed(1800);
    s.pump.requested = 2400; // Local controller or equipment reset changes RPM.
    s.advance(6000); s.confirmed(1800);
    assert(s.pump.writes.size() == 3);
    s.pump.holdWrongSpeed = true;
    s.advance(20000);
    assert(s.pump.writes.size() <= 7);
    for (size_t i = 3; i < s.pump.writes.size(); ++i)
        assert(uint32_t(s.pump.writes[i].at - s.pump.writes[i - 1].at) >= 5000);
}
void stopSupersedesStartup() {
    Scenario s;
    s.target(true, 1800, 1); s.advance(1000);
    s.target(false, 2100, 2); s.advance(1000);
    assert(s.pump.writes.size() == 2 && s.pump.writes.back().value == 1);
    s.advance(65000);
    assert(s.pump.writes.size() == 2 && !s.pump.running);
    assert(s.driver.readState().phase == PoolCommandPhase::Applied);
    s.target(true, 2100, 3); s.advance(62000); s.confirmed(2100);
    assert(s.pump.writes.size() == 3);
}
void offlineStopWins() {
    Scenario s;
    s.target(true, 1800, 1); s.advance(62000);
    s.pump.offline = true; s.advance(6000);
    s.target(false, 1800, 2); s.advance(1000);
    s.pump.running = false;
    s.pump.offline = false; s.advance(10000);
    assert(!s.pump.running && s.driver.readState().phase == PoolCommandPhase::Applied);
    for (size_t i = 1; i < s.pump.writes.size(); ++i) assert(s.pump.writes[i].value == 1);
}
void failedStartExpires() {
    Scenario s;
    s.pump.rejectStart = true;
    s.target(true, 1800, 1); s.advance(20000);
    assert(s.driver.readState().phase == PoolCommandPhase::Expired);
    assert(s.pump.writes.size() == 1);
}
void controllerReboot(bool stoppedDuringReboot) {
    Scenario s;
    s.target(true, 1800, 1); s.advance(62000); s.confirmed(1800);
    s.pump.queued = false;
    if (stoppedDuringReboot) s.pump.running = false;
    s.driver = SerialDeviceDriver{};
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
    assert(s.driver.begin(config, nullptr, &s.pump.service, 1));
    s.target(true, 1800, 1); s.advance(2000);
    assert(s.pump.writes.size() == 2 && s.pump.writes.back().value == 1800);
    if (stoppedDuringReboot) {
        assert(s.driver.readState().phase == PoolCommandPhase::Confirming);
        s.advance(60000);
    }
    s.confirmed(1800);
    assert(s.pump.writes.size() == 2);
}
int main() {
    startupAndNewTarget(100);
    startupAndNewTarget(UINT32_MAX - 20000);
    lostAcknowledgement(); physicalRestartAndDrift(); stopSupersedesStartup();
    offlineStopWins(); failedStartExpires();
    controllerReboot(false); controllerReboot(true);
    puts("Pump startup, restart, delayed feedback, lost ACK, drift, stop and rollover passed");
}

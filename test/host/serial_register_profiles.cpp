#include <stdint.h>
uint32_t testNowMs = 0;
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "Modules/PoolDeviceModule/Drivers/PoolSerialProfiles.h"
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"

struct Bus {
    std::vector<ModbusRequest> requests;
    ModbusMasterService service{};
    ModbusResponse response{};
    bool available = false;
    unsigned cancellations = 0;
    Bus() {
        service.ctx = this;
        service.submit = [](void* ctx, const ModbusRequest* r, uint16_t* id) {
            auto& b = *static_cast<Bus*>(ctx);
            b.requests.push_back(*r); *id = uint16_t(b.requests.size()); b.available = false;
            return MODBUS_RESULT_OK;
        };
        service.poll = [](void* ctx, uint16_t, ModbusResponse* r) {
            auto& b = *static_cast<Bus*>(ctx);
            if (!b.available) return MODBUS_RESULT_NOT_READY;
            *r = b.response; b.available = false; return MODBUS_RESULT_OK;
        };
        service.cancelOwner = [](void* ctx, uint8_t) { ++static_cast<Bus*>(ctx)->cancellations; };
    }
    void reply(uint16_t raw = 0, uint8_t result = MODBUS_RESULT_OK) {
        response = {}; response.result = result; response.registerCount = 1;
        response.values[0] = raw; available = true;
    }
};

PoolDriverConfig pump() {
    PoolDriverConfig c;
    c.capabilities.kind = PoolControlKind::Rs485;
    c.capabilities.unit = PoolSetpointUnit::Rpm;
    c.capabilities.minimum = 1200; c.capabilities.maximum = 2900; c.capabilities.startup = 2400;
    auto& s = c.serial;
    s.protocol = RegisterWireProtocol::VendorRegisterRtu; s.address = 0xAA; s.line.baud = 1200;
    s.control = PoolSerialControl::SetpointOrStop;
    s.setpoint = {3001, 0xD0, RegisterOperation::WriteSingle};
    s.feedback = {2001, 0xC3, RegisterOperation::Read, RegisterResponseLayout::AddressByteCount};
    s.hasFeedback = true; s.runningSource = PoolRunningSource::FeedbackThreshold; s.runningThreshold = 1;
    s.stopValue = 1; s.rawStep = 50; s.rawRounding = PoolRawRounding::Down;
    return c;
}

void combinedControl() {
    auto c = pump(); Bus bus; SerialDeviceDriver driver;
    assert(driver.begin(c, nullptr, &bus.service, 1));
    driver.applyTarget({true, 1724}, 1); driver.tick(10, true);
    assert(bus.requests.size() == 1 && bus.requests.back().registerAddress == 3001);
    assert(bus.requests.back().values[0] == 1700 && bus.requests.back().function == 0xD0);
    bus.reply(); driver.tick(11, true);
    assert(driver.readState().applied.setpoint == 1700);
    // The next operation reads RPM; there is no fixed run write overwriting the speed.
    assert(bus.requests.size() == 2 && bus.requests.back().registerAddress == 2001);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_COMMAND);
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
    assert(bus.requests.back().responseLayout == RegisterResponseLayout::AddressByteCount);
    bus.reply(1700); driver.tick(12, true);
    assert(driver.readState().observedValid && driver.readState().observed.running);
    assert(driver.readState().quality == PoolFeedbackQuality::Confirmed);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    driver.applyTarget({false, 1724}, 2); driver.tick(20, true);
    assert(bus.requests.back().values[0] == 1); // stop is never quantized to zero
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(21, true); bus.reply(1); driver.tick(22, true);
    assert(driver.readState().observedValid && !driver.readState().observed.running);
    driver.tick(1022, true); bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(1023, true);
    assert(!driver.readState().observedValid && driver.readState().error == MODBUS_RESULT_TIMEOUT);

    c.serial.hasFeedback = false; Bus refreshBus; SerialDeviceDriver refresh;
    assert(refresh.begin(c, nullptr, &refreshBus.service, 1));
    refresh.applyTarget({true, 1520}, 1); refresh.tick(10, true);
    refreshBus.reply(); refresh.tick(11, true); refresh.tick(1011, true);
    assert(refreshBus.requests.size() == 2 && refreshBus.requests.back().values[0] == 1500);
    refresh.applyTarget({false, 1520}, 2); refresh.tick(1012, true);
    assert(refreshBus.cancellations == 1 && refreshBus.requests.back().values[0] == 1);
}

void signedFeedbackAndLimits() {
    auto c = pump();
    assert(validatePoolDriverConfig(c));
    assert(poolSerialRawSetpoint(c, 1200) == 1200 && poolSerialRawSetpoint(c, 2900) == 2900);
    c.serial.rawRounding = PoolRawRounding::Nearest;
    assert(poolSerialRawSetpoint(c, 1726) == 1750);
    c.serial.stopValue = 1200; assert(!validatePoolDriverConfig(c));
    c.serial.stopValue = 0; c.serial.rawStep = 0; assert(!validatePoolDriverConfig(c));
    c = pump(); c.capabilities.minimum = 1201; c.capabilities.maximum = 1249;
    c.capabilities.startup = 1220; assert(!validatePoolDriverConfig(c));

    c = {}; c.capabilities.kind = PoolControlKind::Rs485; c.capabilities.unit = PoolSetpointUnit::Celsius;
    c.capabilities.minimum = 5; c.capabilities.maximum = 40; c.capabilities.startup = 25;
    c.serial.hasFeedback = true; c.serial.rawPerUnit = 10;
    c.serial.feedbackType = PoolRegisterValueType::Signed16; c.serial.feedbackUnitsPerRaw = .1f;
    Bus bus; SerialDeviceDriver driver; assert(driver.begin(c, nullptr, &bus.service, 1));
    driver.applyTarget({true, 25}, 1); driver.tick(1, true);
    assert(bus.requests.back().values[0] == 250);
    bus.reply(); driver.tick(2, true); bus.reply(); driver.tick(3, true);
    bus.reply(1); driver.tick(4, true); driver.tick(1003, true);
    bus.reply(0xFFCE); driver.tick(1004, true); // -50 raw = -5°C
    assert(driver.readState().observedValid && driver.readState().observed.setpoint == -5);
    driver.tick(7000, false);
    assert(!driver.readState().observedValid && driver.readState().quality == PoolFeedbackQuality::Stale);
}

std::vector<uint8_t> withCrc(std::vector<uint8_t> payload) {
    const auto crc = ModbusRtuCodec::crc16(payload.data(), payload.size());
    payload.push_back(uint8_t(crc)); payload.push_back(uint8_t(crc >> 8)); return payload;
}
void responseLayouts() {
    ModbusRequest r; r.slaveAddress = 0xAA; r.function = 0xC3; r.registerAddress = 2001;
    r.protocol = RegisterWireProtocol::VendorRegisterRtu;
    r.responseLayout = RegisterResponseLayout::AddressByteCount;
    auto frame = withCrc({0xAA, 0xC3, 0x07, 0xD1, 2, 0x06, 0xA4});
    ModbusResponse out;
    assert(ModbusRtuCodec::decodeResponse(r, frame.data(), frame.size(), out) == MODBUS_RESULT_OK);
    assert(out.values[0] == 1700);
    const std::vector<std::vector<uint8_t>> malformed = {
        {0xAA, 0xC3, 0x07, 0xD2, 2, 0x06, 0xA4}, // wrong register, valid CRC
        {0xAB, 0xC3, 0x07, 0xD1, 2, 0x06, 0xA4}, // wrong participant
        {0xAA, 0xC3, 0x07, 0xD1, 4, 0x06, 0xA4}, // incorrect byte count
        {0xAA, 0xC3, 0x07}, // short header
        {0xAA, 0xC3, 0x07, 0xD1, 2, 0x06}, // short value
        {0xAA, 0xC3, 0x07, 0xD1, 2, 0x06, 0xA4, 0} // extra byte
    };
    for (const auto& payload : malformed) {
        const auto bad = withCrc(payload);
        assert(ModbusRtuCodec::decodeResponse(r, bad.data(), bad.size(), out) == MODBUS_RESULT_PROTOCOL_ERROR);
        assert(out.registerCount == 0);
    }
    frame.back() ^= 1;
    assert(ModbusRtuCodec::decodeResponse(r, frame.data(), frame.size(), out) == MODBUS_RESULT_CRC_ERROR);
    r.operation = RegisterOperation::WriteSingle; r.function = 0xD0; r.registerAddress = 3001; r.values[0] = 1700;
    frame = withCrc({0xAA, 0xD0, 0x0B, 0xB9, 2, 0x06, 0xA4});
    assert(ModbusRtuCodec::decodeResponse(r, frame.data(), frame.size(), out) == MODBUS_RESULT_OK);
    r.values[0] = 1750;
    assert(ModbusRtuCodec::decodeResponse(r, frame.data(), frame.size(), out) == MODBUS_RESULT_PROTOCOL_ERROR);
    r.operation = RegisterOperation::WriteMultiple; assert(!ModbusRtuCodec::validRequest(r));
    r.protocol = RegisterWireProtocol::ModbusRtu; r.function = 6; assert(!ModbusRtuCodec::validRequest(r));
    r.responseLayout = RegisterResponseLayout::Standard; assert(ModbusRtuCodec::validRequest(r));
}

void modesAndTelemetry() {
    PoolDriverConfig c; c.capabilities.kind = PoolControlKind::Rs485;
    c.capabilities.unit = PoolSetpointUnit::Celsius;
    c.capabilities.minimum = 5; c.capabilities.maximum = 40; c.capabilities.startup = 25;
    auto& s = c.serial;
    s.modes.count = 2; s.modes.options[0] = {"Eco", 0x78}; s.modes.options[1] = {"Smart", 0x58};
    s.hasFeedback = true; s.rawPerUnit = 10; s.feedbackUnitsPerRaw = .1f;
    s.telemetryProfile = PoolTelemetryProfile::HeatPumpPoly;
    s.pollMs = 50; s.staleMs = 1000; s.telemetryStaleMs = 1000;
    assert(validatePoolDriverConfig(c));
    assert(!validatePoolTarget(c, {true, 25, 2}));
    Bus bus; SerialDeviceDriver driver; assert(driver.begin(c, nullptr, &bus.service, 1));
    driver.applyTarget({true, 25, 1}, 1); driver.tick(1, true);
    bus.reply(); driver.tick(2, true);
    assert(bus.requests.back().values[0] == 0x58);
    bus.reply(); driver.tick(3, true);
    assert(driver.readState().applied.mode == 1);
    size_t handled = 2;
    bool sawTemperatureBlock = false, sawFaultBlock = false;
    for (uint32_t now = 4; now <= 650; ++now) {
        if (handled < bus.requests.size()) {
            const auto& r = bus.requests[handled++];
            bus.reply(r.registerAddress == 0 ? 1 : 250);
            bus.response.registerCount = r.registerCount;
            if (r.registerAddress == 510) {
                sawTemperatureBlock = true;
                assert(r.registerCount == 7 && r.priority == MODBUS_PRIORITY_BACKGROUND);
                for (uint8_t i = 0; i < r.registerCount; ++i) bus.response.values[i] = 0xFFCE;
            } else if (r.registerAddress == 521) {
                sawFaultBlock = true; bus.response.result = MODBUS_RESULT_EXCEPTION;
            } else if (r.registerAddress == 503) bus.response.values[0] = 0x8000;
        }
        driver.tick(now, true);
    }
    const auto& state = driver.readState();
    assert(sawTemperatureBlock && sawFaultBlock);
    assert(state.observedValid && state.error == 0 && state.appliedValid);
    assert((state.telemetry.validBlocks & (1U << 2)) && !(state.telemetry.validBlocks & (1U << 3)));
    assert(poolTelemetryValue(state.telemetry, kHeatPumpPolyFields[9]) == -5);
    assert(poolTelemetryValue(state.telemetry, kHeatPumpPolyFields[8]) == 1);
    driver.tick(2000, false);
    assert(driver.readState().telemetry.validBlocks == 0);
    const size_t previousRequests = bus.requests.size();
    driver.applyTarget({false, 25, 0}, 2); driver.tick(2001, true);
    assert(bus.requests.size() == previousRequests + 1 && bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    assert(bus.requests.back().values[0] == s.stopValue);
    s.telemetryStaleMs = 799; assert(!validatePoolDriverConfig(c));
    s.telemetryStaleMs = 1000; s.modes.options[1].value = s.stopValue;
    assert(!validatePoolDriverConfig(c));
}


void manufacturerProfiles() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
    assert(validatePoolDriverConfig(config));
    assert(config.capabilities.startup == 2900);
    Bus bus; SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({true, 1700}, 1);
    driver.tick(10, true);
    assert(bus.requests.size() == 1);
    assert(bus.requests.back().operation == RegisterOperation::Read);
    bus.reply(0); bus.response.aquaGem = {true, 0, 0, 0};
    driver.tick(11, true);
    driver.tick(60011, true);
    assert(bus.requests.size() == 2);
    assert(bus.requests.back().operation == RegisterOperation::WriteSingle);
    assert(bus.requests.back().values[0] == 1700);
    uint8_t frame[16]{}; size_t length = 0;
    const auto write = bus.requests.back();
    assert(ModbusRtuCodec::encodeRequest(write, frame, sizeof(frame), length));
    const uint8_t expectedWrite[] = {0xAA,0xD0,0x0B,0xB9,0x06,0xA4,0x08,0x19};
    assert(length == sizeof(expectedWrite) && std::equal(frame, frame + length, expectedWrite));
    bus.reply(); driver.tick(60012, true);
    driver.tick(60511, true);
    assert(bus.requests.back().operation == RegisterOperation::Read);
    bus.reply(1700); bus.response.aquaGem = {true, 0, 1, 1700};
    driver.tick(60512, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(driver.readState().observed.setpoint == 1700);
    driver.applyTarget({false, 1700}, 2);
    driver.tick(61011, true);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    assert(bus.requests.back().values[0] == 1);

    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    assert(validatePoolDriverConfig(config));
    Bus poly; SerialDeviceDriver pac;
    assert(pac.begin(config, nullptr, &poly.service, 1));
    pac.applyTarget({true, 28, 0}, 1);
    pac.tick(1, true);
    assert(poly.requests.back().registerAddress == 58 && poly.requests.back().function == 3);
}

void stoppedHeatPumpSetpoint() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 8));
    driver.applyTarget({false, 29, 0}, 1);
    driver.tick(1, true);
    assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(2, true);
    assert(driver.readState().phase == PoolCommandPhase::Pending);
    assert(bus.requests.back().registerAddress == 58);
    bus.reply(400); driver.tick(3, true);
    assert(bus.requests.back().function == 6 && bus.requests.back().registerAddress == 1001);
    assert(bus.requests.back().values[0] == 290);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_COMMAND);
    bus.reply(); driver.tick(4, true);
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
    assert(!driver.readState().applied.running && driver.readState().applied.setpoint == 29);
    assert(!driver.readState().observedValid); // Acknowledgement is not physical readback.
    assert(bus.requests.back().priority == MODBUS_PRIORITY_COMMAND);
    bus.reply(0); driver.tick(5, true); // Status: still off.
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_COMMAND);
    assert(bus.requests.back().registerAddress == 1001 && bus.requests.back().function == 3);
    bus.reply(290); driver.tick(6, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(driver.readState().observedValid && !driver.readState().observed.running);
    assert(driver.readState().observed.setpoint == 29);
    // An invalid temperature must not prevent the stop, nor be written afterward.
    driver.applyTarget({false, 45, 0}, 2); driver.tick(2010, true);
    assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
    bus.reply(); driver.tick(2011, true);
    assert(bus.requests.back().registerAddress == 58);
    const size_t beforeLimit = bus.requests.size();
    bus.reply(400); driver.tick(2012, true);
    assert(bus.requests.size() == beforeLimit);
    assert(driver.readState().phase == PoolCommandPhase::Failed);
    // A rejected write must not report the new temperature as applied.
    driver.applyTarget({false, 30, 0}, 3); driver.tick(2020, true);
    bus.reply(); driver.tick(2021, true);
    bus.reply(400); driver.tick(2022, true);
    assert(bus.requests.back().registerAddress == 1001);
    bus.reply(0, MODBUS_RESULT_EXCEPTION); driver.tick(2023, true);
    assert(driver.readState().phase == PoolCommandPhase::Failed);
    assert(!driver.readState().appliedValid);
    driver.tick(3023, true);
    assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
    for (const auto& request : bus.requests) {
        if (request.function == 6 && request.registerAddress == 1000) assert(request.values[0] == 0);
    }
}

void heatPumpLimitWireUnits() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    // Replay the exact register-58 reply from the failing boot, through the codec.
    for (bool running : {false, true}) {
        for (float target : {32.0f, 32.1f}) {
            Bus bus;
            SerialDeviceDriver driver;
            assert(driver.begin(config, nullptr, &bus.service, 8));
            driver.applyTarget({running, target, 0}, 1);
            driver.tick(1, true);
            if (!running) {
                assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
                bus.reply(); driver.tick(2, true);
            }
            assert(bus.requests.back().registerAddress == 58 && bus.requests.back().function == 3);
            const uint8_t reply[] = {0x11, 0x03, 0x02, 0x01, 0x40, 0x79, 0xE7};
            assert(ModbusRtuCodec::decodeResponse(bus.requests.back(), reply, sizeof(reply), bus.response) == MODBUS_RESULT_OK);
            assert(bus.response.registerCount == 1 && bus.response.values[0] == 320);
            bus.response.result = MODBUS_RESULT_OK;
            bus.available = true;
            const size_t beforeLimit = bus.requests.size();
            driver.tick(3, true);
            if (target > 32.0f) {
                assert(bus.requests.size() == beforeLimit);
                assert(driver.readState().error == MODBUS_RESULT_INVALID_ARGUMENT);
                continue;
            }
            assert(bus.requests.back().registerAddress == 1001 && bus.requests.back().values[0] == 320);
            bus.reply(); driver.tick(4, true);
            if (running) {
                assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0x15);
                bus.reply(); driver.tick(5, true);
            }
            assert(driver.readState().phase == PoolCommandPhase::Confirming);
            assert(driver.readState().error == 0);
            assert(driver.readState().applied.running == running);
            assert(bus.requests.back().function == 3 && bus.requests.back().registerAddress == 1000);
        }
    }
    // Boot with the default stopped target, including boundary and malformed limits.
    for (uint16_t raw : {0, 40, 249, 250, 320, 600, 601, 65535}) {
        Bus bus;
        SerialDeviceDriver driver;
        assert(driver.begin(config, nullptr, &bus.service, 8));
        driver.tick(1, true);
        assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
        bus.reply(); driver.tick(2, true);
        assert(bus.requests.back().registerAddress == 58);
        const size_t beforeLimit = bus.requests.size();
        bus.reply(raw); driver.tick(3, true);
        if (raw < 250 || raw > 600) {
            assert(bus.requests.size() == beforeLimit);
            assert(driver.readState().error == MODBUS_RESULT_PROTOCOL_ERROR);
        } else {
            assert(bus.requests.back().registerAddress == 1001 && bus.requests.back().values[0] == 250);
            bus.reply(); driver.tick(4, true);
            assert(driver.readState().phase == PoolCommandPhase::Confirming);
            assert(!driver.readState().applied.running);
        }
    }
}

void authoritativeHeatPumpTemperature() {
    for (bool running : {false, true}) {
        PoolDriverConfig config;
        assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
        config.serial.telemetryProfile = PoolTelemetryProfile::None;
        config.serial.pollMs = 50;
        Bus bus;
        SerialDeviceDriver driver;
        assert(driver.begin(config, nullptr, &bus.service, 8));
        driver.applyTarget({running, 29.0f, 2}, 1);
        uint16_t temperature = 250, status = 0;
        unsigned temperatureWrites = 0, processed = 0;
        uint32_t now = 0;
        auto advance = [&](unsigned ticks, bool enabled = true) {
            for (unsigned i = 0; i < ticks; ++i) {
                now += 100;
                driver.tick(now, enabled);
                if (processed == bus.requests.size()) continue;
                const auto& request = bus.requests[processed++];
                if (request.function == 6) {
                    if (request.registerAddress == 1001) {
                        temperature = request.values[0];
                        ++temperatureWrites;
                    } else {
                        assert(request.registerAddress == 1000);
                        status = request.values[0];
                        assert(running || status == 0); // temperature never starts a stopped PAC
                    }
                    bus.reply();
                } else {
                    assert(request.function == 3);
                    bus.reply(request.registerAddress == 58 ? 320 :
                        request.registerAddress == 1000 ? status : temperature);
                }
            }
        };
        advance(40);
        assert(temperature == 290 && temperatureWrites == 1);
        assert(driver.readState().phase == PoolCommandPhase::Applied);
        const auto writes = temperatureWrites;
        advance(40);
        assert(temperatureWrites == writes); // no flash or bus writes while matching
        temperature = 250; // physical keypad wins after startup confirmation
        advance(80);
        assert(temperature == 250 && temperatureWrites == writes);
        assert(driver.readState().phase == PoolCommandPhase::Applied);
        assert(driver.readState().observed.running == running);
        driver.applyTarget({running, 30.0f, 2}, 2);
        advance(40);
        assert(temperature == 300 && driver.readState().applied.mode == 2);
        temperature = 250;
        advance(40, false);
        assert(temperature == 250); // freeze never overwrites the keypad
        advance(80);
        assert(temperature == 250);
        // An outage and reconnect must not replay an old target.
        temperature = 260;
        bus.reply(0, MODBUS_RESULT_TIMEOUT);
        advance(120);
        assert(temperature == 260 && driver.readState().observed.running == running);
    }
}

void confirmationFailureAndReplacement() {
    auto config = pump();
    config.serial.line.minimumRequestIntervalMs = 500;
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({true, 1700}, 1); driver.tick(10, true);
    bus.reply(); driver.tick(11, true);
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
    driver.tick(509, true); assert(bus.requests.size() == 1);
    driver.tick(510, true);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_COMMAND);
    bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(511, true);
    assert(driver.readState().communicationDegraded && driver.readState().phase == PoolCommandPhase::Confirming);
    assert(!driver.readState().observedValid);
    driver.applyTarget({true, 1800}, 2); driver.tick(1010, true);
    assert(bus.requests.back().registerAddress == 2001); // Reconnect read before changing RPM.
    bus.reply(); driver.tick(1011, true); driver.tick(1510, true);
    assert(bus.requests.back().values[0] == 1800);
    const auto cancelled = bus.cancellations;
    driver.applyTarget({false, 1800}, 3);
    assert(bus.cancellations == cancelled + 1);
    driver.tick(2010, true);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(2011, true);
    const auto count = bus.requests.size();
    driver.tick(2510, false); assert(bus.requests.size() == count);
    driver.tick(2511, true);
    assert(bus.requests.back().registerAddress == 2001);
    bus.reply(0); driver.tick(2512, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(!driver.readState().observed.running);
}

void communicationRecoveryBackoff() {
    Bus bus;
    SerialDeviceDriver driver;
    auto config = pump();
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({true, 2200}, 1);
    uint32_t now = 10;
    driver.tick(now, true);
    for (uint32_t delay : {1000U, 2000U, 4000U, 8000U, 8000U}) {
        bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(++now, true);
        const auto count = bus.requests.size();
        driver.tick(now + delay - 1, true);
        assert(bus.requests.size() == count);
        now += delay; driver.tick(now, true);
        assert(bus.requests.size() == count + 1);
        assert(bus.requests.back().priority == MODBUS_PRIORITY_NORMAL);
    }
    bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(++now, true);
    const auto count = bus.requests.size();
    assert(driver.readState().phase == PoolCommandPhase::Expired);
    // A new stop bypasses recovery delay and retains safety priority.
    driver.applyTarget({false, 2200}, 2); driver.tick(++now, true);
    assert(bus.requests.size() == count + 1 && bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(++now, true);
    bus.reply(0); driver.tick(++now, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    driver.applyTarget({true, 2300}, 3); driver.tick(++now, true);
    bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(++now, true);
    const auto recovered = bus.requests.size();
    driver.tick(now + 999, true); assert(bus.requests.size() == recovered);
    driver.tick(now + 1000, true); assert(bus.requests.size() == recovered + 1);
}


void aquaGemWarmupAndNearSpeedReadback() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
    Bus bus; SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));

    // The physical pump is at 1650 RPM. Flow.io asks for 1670 RPM, a
    // difference smaller than the measured-speed tolerance. Nothing may
    // time out before the 60-second power-on hold has ended.
    driver.applyTarget({true, 1670}, 1);
    size_t handled = 0;
    unsigned writes = 0;
    uint16_t lastWritten = 0;
    for (uint32_t now = 100; now <= 63000; now += 100) {
        if (handled < bus.requests.size()) {
            const ModbusRequest& request = bus.requests[handled++];
            if (request.operation == RegisterOperation::WriteSingle) {
                ++writes;
                lastWritten = request.values[0];
                bus.reply();
            } else {
                bus.reply(1650);
                bus.response.aquaGem = {true, 0, 1, 1650};
            }
        }
        driver.tick(now, true);
        if (now < 60200) {
            assert(writes == 0);
            assert(driver.readState().phase != PoolCommandPhase::Expired);
        }
    }
    assert(writes == 1 && lastWritten == 1670);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(driver.readState().applied.setpoint == 1670);
    assert(driver.readState().observed.setpoint == 1650);
    // The physical measurement is not incorrectly substituted for the
    // accepted requested speed.
}

void delayedPumpApplication() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({true, 1800}, 1);
    const uint32_t startedAt = 100;
    uint16_t acceptedRpm = 0;
    unsigned writes = 0;
    size_t handled = 0;
    for (uint32_t now = startedAt; now < startedAt + 62000; ++now) {
        // A second setpoint must replace the pending command without restarting
        // the equipment's fixed 60-second startup period or resending the first.
        if (now == startedAt + 30000) driver.applyTarget({true, 2000}, 2);
        if (handled < bus.requests.size()) {
            const auto& request = bus.requests[handled++];
            if (request.operation == RegisterOperation::WriteSingle) {
                acceptedRpm = request.values[0];
                ++writes;
                bus.reply();
            } else {
                const uint16_t actual = now < startedAt + 60000 ? 2900 : acceptedRpm;
                bus.reply(actual);
                bus.response.aquaGem = {true, 0, 1, actual};
            }
        }
        driver.tick(now, true);
        const auto& state = driver.readState();
        if (now > startedAt + 1000 && now < startedAt + 60000) {
            assert(state.observedValid && state.observed.setpoint == 2900);
            assert(state.observed.running); // ON is confirmed before RPM convergence.
            assert(state.quality == PoolFeedbackQuality::Confirmed);
            assert(state.phase == PoolCommandPhase::Pending || state.phase == PoolCommandPhase::Confirming);
            assert(state.error == 0);
        }
    }
    assert(writes == 1); // queued command superseded during AquaGem warmup
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(driver.readState().appliedRevision == 2);
    assert(driver.readState().applied.setpoint == 2000);
    assert(driver.readState().observed.setpoint == 2000);
    // A later deviation is still a valid reading, but no longer convergence.
    driver.tick(startedAt + 63000, true);
    bus.reply(2900); bus.response.aquaGem = {true, 0, 1, 2900};
    driver.tick(startedAt + 63001, true);
    assert(driver.readState().quality == PoolFeedbackQuality::Confirmed);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(writes == 1); // later physical changes are not repaired by Flow.io
}

void stoppedSeparateSetpointMustConverge() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({false, 29}, 1);
    driver.tick(1, true);
    bus.reply(); driver.tick(2, true); // stop acknowledged
    bus.reply(400); driver.tick(3, true); // temperature limit
    bus.reply(); driver.tick(4, true); // setpoint acknowledged
    bus.reply(0); driver.tick(5, true); // stopped status
    bus.reply(280); driver.tick(6, true); // old setpoint is still reported
    assert(driver.readState().quality == PoolFeedbackQuality::Confirmed);
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
    size_t handled = bus.requests.size();
    for (uint32_t now = 7; now < 5000; ++now) {
        if (handled < bus.requests.size()) {
            const auto& request = bus.requests[handled++];
            assert(request.operation == RegisterOperation::Read);
            bus.reply(request.registerAddress == 1001 ? 290 : 0);
            bus.response.registerCount = request.registerCount;
        }
        driver.tick(now, true);
    }
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(driver.readState().observed.setpoint == 29);
}

void heatPumpEcoRestartAndElectricalTelemetry() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    uint32_t now = 1;
    for (uint32_t revision = 1; revision <= 3; ++revision) {
        const bool running = revision != 2;
        driver.applyTarget({running, 28, 2}, revision); // Heating Eco, including restart.
        driver.tick(now++, true);
        if (!running) {
            assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 0);
            bus.reply(); driver.tick(now++, true);
        }
        assert(bus.requests.back().registerAddress == 58);
        bus.reply(400); driver.tick(now++, true);
        assert(bus.requests.back().registerAddress == 1001 && bus.requests.back().values[0] == 280);
        bus.reply(); driver.tick(now++, true);
        if (running) {
            assert(bus.requests.back().registerAddress == 1000 && bus.requests.back().values[0] == 23);
            bus.reply(); driver.tick(now++, true);
        }
        bus.reply(running ? 23 : 0); driver.tick(now++, true);
        bus.reply(280); driver.tick(now++, true);
        assert(driver.readState().phase == PoolCommandPhase::Applied);
        assert(driver.readState().observed.running == running);
    }
    size_t handled = bus.requests.size();
    bool electricalRead = false;
    for (; now < 12000 && !electricalRead; ++now) {
        if (handled < bus.requests.size()) {
            const auto request = bus.requests[handled++];
            assert(request.operation == RegisterOperation::Read);
            bus.reply(request.registerAddress == 1000 ? 23 : 280);
            bus.response.registerCount = request.registerCount;
            if (request.registerAddress == 521) {
                bus.response.values[0] = 230;
                bus.response.values[1] = 45;
                bus.response.values[2] = 0;
                electricalRead = true;
            }
        }
        driver.tick(now, true);
    }
    assert(electricalRead);
    const auto telemetry = driver.readState().telemetry;
    assert(telemetry.raw[10] == 45); // Preserve wire data for diagnostics.
    assert(poolTelemetryValue(telemetry, kHeatPumpPolyFields[16]) == 230);
    assert(std::fabs(poolTelemetryValue(telemetry, kHeatPumpPolyFields[17]) - 4.5f) < .001f);
}


void lostAckAndStopWhileDisconnected() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, config));
    Bus bus; SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 1));
    driver.applyTarget({true, 2000}, 1);
    driver.tick(1, true);
    assert(bus.requests.back().operation == RegisterOperation::Read);
    bus.reply(0); bus.response.aquaGem = {true, 0, 0, 0};
    driver.tick(2, true);
    driver.tick(60002, true);
    assert(bus.requests.back().operation == RegisterOperation::WriteSingle);
    assert(bus.requests.back().retries == 0);
    bus.reply(0, MODBUS_RESULT_TIMEOUT);
    driver.tick(60003, true);
    driver.tick(61003, true);
    assert(bus.requests.back().operation == RegisterOperation::Read);
    bus.reply(2000); bus.response.aquaGem = {true, 0, 1, 2000};
    driver.tick(61004, true);
    driver.tick(61503, true);
    assert(bus.requests.back().operation == RegisterOperation::Read);
    bus.reply(2000); bus.response.aquaGem = {true, 0, 1, 2000};
    driver.tick(61504, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    driver.tick(62003, true);
    bus.reply(2500); bus.response.aquaGem = {true, 0, 1, 2500};
    driver.tick(62004, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    // An external change never produces a repair write.
    for (size_t i = 2; i < bus.requests.size(); ++i)
        assert(bus.requests[i].operation == RegisterOperation::Read);
    driver.applyTarget({false, 2000}, 2);
    driver.tick(62503, true);
    assert(bus.requests.back().values[0] == 1);
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(62504, true);
    driver.tick(63003, true);
    bus.reply(0); bus.response.aquaGem = {true, 0, 0, 0};
    driver.tick(63004, true);
    assert(driver.readState().phase == PoolCommandPhase::Applied);
}

void wrongHeatModeNotConfirmed() {
    PoolDriverConfig config;
    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, config));
    Bus bus;
    SerialDeviceDriver driver;
    assert(driver.begin(config, nullptr, &bus.service, 8));
    driver.applyTarget({true, 28, 2}, 1); driver.tick(1, true);
    bus.reply(320); driver.tick(2, true);
    bus.reply(); driver.tick(3, true);
    assert(bus.requests.back().values[0] == 23);
    bus.reply(); driver.tick(4, true);
    bus.reply(17); driver.tick(5, true);
    bus.reply(280); driver.tick(6, true);
    assert(driver.readState().observedValid && driver.readState().observed.running);
    assert(driver.readState().phase == PoolCommandPhase::Confirming);
}

int main() {
#ifdef FLOW_TEST_AUTHORITATIVE_HEATPUMP
    authoritativeHeatPumpTemperature();
#else
    lostAckAndStopWhileDisconnected();
    wrongHeatModeNotConfirmed();
    heatPumpEcoRestartAndElectricalTelemetry();
    delayedPumpApplication();
    aquaGemWarmupAndNearSpeedReadback();
    stoppedSeparateSetpointMustConverge();
    communicationRecoveryBackoff();
    confirmationFailureAndReplacement();
    authoritativeHeatPumpTemperature();
    heatPumpLimitWireUnits();
    stoppedHeatPumpSetpoint();
    manufacturerProfiles();
    combinedControl(); signedFeedbackAndLimits(); responseLayouts(); modesAndTelemetry();
#endif
    puts("serial register profile tests passed");
}

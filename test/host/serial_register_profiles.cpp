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
    assert(bus.requests.back().responseLayout == RegisterResponseLayout::AddressByteCount);
    bus.reply(1700); driver.tick(12, true);
    assert(driver.readState().observedValid && driver.readState().observed.running);
    assert(driver.readState().quality == PoolFeedbackQuality::Confirmed);
    driver.applyTarget({false, 1724}, 2); driver.tick(20, true);
    assert(bus.requests.back().values[0] == 1); // stop is never quantized to zero
    assert(bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    bus.reply(); driver.tick(21, true); bus.reply(1); driver.tick(22, true);
    assert(driver.readState().observedValid && !driver.readState().observed.running);
    driver.tick(1021, true); bus.reply(0, MODBUS_RESULT_TIMEOUT); driver.tick(1022, true);
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
    PoolDriverConfig c;
    assert(applyPoolSerialProfile(PoolSerialModel::AquaGem, c));
    assert(validatePoolDriverConfig(c));
    Bus bus; SerialDeviceDriver driver;
    assert(driver.begin(c, nullptr, &bus.service, 1));
    driver.applyTarget({true, 1700}, 1); driver.tick(10, true);
    const auto write = bus.requests.back();
    uint8_t frame[16]{}; size_t length = 0;
    assert(ModbusRtuCodec::encodeRequest(write, frame, sizeof(frame), length));
    const uint8_t expectedWrite[] = {0xAA,0xD0,0x0B,0xB9,0x06,0xA4,0x08,0x19};
    assert(length == sizeof(expectedWrite) && std::equal(frame, frame + length, expectedWrite));
    const uint8_t ack[] = {0xAA,0xD0,0x0B,0xB9,0,0,0,0x82,0x07};
    ModbusResponse response;
    assert(ModbusRtuCodec::decodeResponse(write, ack, sizeof(ack), response) == MODBUS_RESULT_OK);
    bus.reply(); driver.tick(11, true);
    driver.tick(509, true); assert(bus.requests.size() == 1);
    driver.tick(510, true); assert(bus.requests.size() == 2);
    const auto read = bus.requests.back();
    assert(ModbusRtuCodec::encodeRequest(read, frame, sizeof(frame), length));
    const uint8_t expectedRead[] = {0xAA,0xC3,0x07,0xD1,0,0,0x0D,0x4D};
    assert(length == sizeof(expectedRead) && std::equal(frame, frame + length, expectedRead));
    const uint8_t feedback[] = {0xAA,0xC3,0,0x10,1,0x06,0xA4,0x57,0x22};
    assert(ModbusRtuCodec::decodeResponse(read, feedback, sizeof(feedback), response) == MODBUS_RESULT_OK);
    assert(response.aquaGem.valid && response.aquaGem.faults == 16 && response.aquaGem.rpm == 1700);
    bus.reply(); bus.response = response; bus.response.result = MODBUS_RESULT_OK;
    driver.tick(511, true);
    assert(driver.readState().observedValid && driver.readState().observed.running);
    assert(driver.readState().deviceFaults == 16 && driver.readState().observed.setpoint == 1700);
    // Reject truncated frames, reversed CRC, and valid-CRC replies from a different slave.
    assert(ModbusRtuCodec::decodeResponse(read, feedback, 8, response) == MODBUS_RESULT_PROTOCOL_ERROR);
    auto bad = std::vector<uint8_t>(feedback, feedback + sizeof(feedback));
    std::swap(bad[7], bad[8]);
    assert(ModbusRtuCodec::decodeResponse(read, bad.data(), bad.size(), response) == MODBUS_RESULT_CRC_ERROR);
    bad[0] = 0xAB;
    auto crc = ModbusRtuCodec::crc16(bad.data(), 7); bad[7] = crc & 255; bad[8] = crc >> 8;
    assert(ModbusRtuCodec::decodeResponse(read, bad.data(), bad.size(), response) == MODBUS_RESULT_PROTOCOL_ERROR);
    driver.applyTarget({false,1700}, 2); driver.tick(1010,true);
    assert(bus.requests.back().values[0] == 1 && bus.requests.back().priority == MODBUS_PRIORITY_SAFETY);
    assert(ModbusRtuCodec::encodeRequest(bus.requests.back(),frame,sizeof(frame),length));
    const uint8_t expectedStop[] = {0xAA,0xD0,0x0B,0xB9,0,1,0xCB,0xC2};
    assert(std::equal(frame,frame+length,expectedStop));
    auto invalid = write; invalid.values[0] = 0; assert(!ModbusRtuCodec::validRequest(invalid));
    invalid = read; invalid.registerAddress = 2002; assert(!ModbusRtuCodec::validRequest(invalid));
    // Device status remains authoritative even if measured RPM is zero.
    driver.tick(1011,true); // wait for stop acknowledgement
    bus.reply(); driver.tick(1012,true); driver.tick(1510,true);
    bus.reply(0); bus.response.aquaGem = {true,0,1,0}; driver.tick(1511,true);
    assert(driver.readState().observedValid && driver.readState().observed.running);

    assert(applyPoolSerialProfile(PoolSerialModel::Polytropic, c));
    assert(validatePoolDriverConfig(c));
    Bus poly; SerialDeviceDriver pac;
    assert(pac.begin(c,nullptr,&poly.service,1));
    pac.applyTarget({true,28,0},1); pac.tick(1,true);
    assert(poly.requests.back().registerAddress == 58 && poly.requests.back().function == 3);
    poly.reply(400); pac.tick(2,true);
    assert(poly.requests.back().registerAddress == 1001 && poly.requests.back().values[0] == 280);
    poly.reply(); pac.tick(3,true);
    assert(poly.requests.back().registerAddress == 1000 && poly.requests.back().values[0] == 0x15);
    poly.reply(); pac.tick(4,true);
    assert(poly.requests.back().registerAddress == 1000 && poly.requests.back().function == 3);
    pac.applyTarget({true,45,2},2); pac.tick(5,true);
    assert(poly.requests.back().registerAddress == 58);
    poly.reply(400); pac.tick(6,true);
    assert(pac.readState().error == MODBUS_RESULT_INVALID_ARGUMENT);
    assert(poly.requests.back().function == 3); // no out-of-range write
    pac.applyTarget({false,45,2},3); pac.tick(7,true);
    assert(poly.requests.back().function == 6 && poly.requests.back().values[0] == 0);
    for (uint8_t i=0;i<c.serial.modes.count;++i) assert((c.serial.modes.options[i].value & 0xFFE0) == 0);
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
    assert(driver.readState().phase == PoolCommandPhase::Applied);
    assert(!driver.readState().applied.running && driver.readState().applied.setpoint == 29);
    assert(!driver.readState().observedValid); // Acknowledgement is not physical readback.
    bus.reply(0); driver.tick(5, true); // Status: still off.
    driver.tick(1005, true);
    bus.reply(); driver.tick(1006, true); // Telemetry precedes the next feedback read.
    driver.tick(2005, true);
    assert(bus.requests.back().registerAddress == 1001 && bus.requests.back().function == 3);
    bus.reply(290); driver.tick(2006, true);
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
            assert(driver.readState().phase == PoolCommandPhase::Applied);
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
            assert(driver.readState().phase == PoolCommandPhase::Applied);
            assert(!driver.readState().applied.running);
        }
    }
}

int main() {
    heatPumpLimitWireUnits();
    stoppedHeatPumpSetpoint();
    manufacturerProfiles();
    combinedControl(); signedFeedbackAndLimits(); responseLayouts(); modesAndTelemetry();
    puts("serial register profile tests passed");
}
#include <cassert>
#include <cstdio>
#include "Modules/IOModule/IOBus/Rs485Bus.h"
#include "Modules/IOModule/IOScheduler/Rs485TransactionScheduler.h"

uint32_t testNowMs = 0;

void exercise(uint32_t baud, uint32_t start, unsigned responseBytes,
              ModbusResultCode expected, bool aquaGem = false)
{
    testNowMs = start;
    HardwareSerial serial;
    Rs485Bus bus(serial);
    UartSpec spec{"test", 1, 1, 2, baud, false, -1};
    assert(bus.begin(spec));
    Rs485TransactionScheduler scheduler(bus);
    assert(scheduler.begin());
    auto& service = scheduler.service();
    ModbusRequest request;
    request.ownerId = 1;
    request.function = 6;
    request.line.baud = baud;
    request.responseTimeoutMs = 500;
    request.retries = 0;
    if (aquaGem) {
        request.protocol = RegisterWireProtocol::AquaGem;
        request.operation = RegisterOperation::Read;
        request.slaveAddress = 0xAA;
        request.function = 0xC3;
        request.registerAddress = 2001;
        request.registerCount = 1;
    }
    uint16_t id = 0;
    assert(service.submit(service.ctx, &request, &id) == MODBUS_RESULT_OK);
    auto tick = [&](uint32_t offset) {
        testNowMs = start + offset;
        scheduler.tick(testNowMs, micros());
    };
    tick(100);
    assert(serial.transmitted.size() == 8);
    tick(200); // UART transmission has finished.
    // Actual nine-byte AquaGem status response from the reported incident.
    const std::vector<uint8_t> reply = aquaGem
        ? std::vector<uint8_t>{0xAA, 0xC3, 0, 0, 1, 0x0B, 0x54, 0x57, 0x36}
        : serial.transmitted;
    assert(responseBytes <= reply.size());
    serial.received.insert(serial.received.end(), reply.begin(), reply.begin() + responseBytes);
    tick(1200); // First UART drain after a one-second scheduling stall.
    ModbusResponse response;
    if (responseBytes) {
        assert(bus.receivingFrame());
        assert(service.poll(service.ctx, id, &response) == MODBUS_RESULT_NOT_READY);
        tick(1201);
        assert(service.poll(service.ctx, id, &response) == MODBUS_RESULT_NOT_READY);
        tick(1240); // More than 3.5 characters even at 1200 baud.
    }
    assert(service.poll(service.ctx, id, &response) == MODBUS_RESULT_OK);
    if (response.result != expected) std::fprintf(stderr, "baud=%u bytes=%u aqua=%d result=%u expected=%u\n", baud, responseBytes, aquaGem, unsigned(response.result), unsigned(expected));
    assert(response.result == expected);
    if (aquaGem && expected == MODBUS_RESULT_OK) {
        assert(response.aquaGem.valid && response.aquaGem.rpm == 2900);
    }
    ModbusMasterStats stats;
    assert(service.stats(service.ctx, &stats));
    assert(stats.timeouts == (expected == MODBUS_RESULT_TIMEOUT ? 1U : 0U));
    assert(stats.retries == 0);
    bus.end();
}

int main()
{
    for (uint32_t baud : {1200U, 9600U}) {
        exercise(baud, 0, 8, MODBUS_RESULT_OK);
        exercise(baud, 0, 0, MODBUS_RESULT_TIMEOUT);
        exercise(baud, 0, 2, MODBUS_RESULT_PROTOCOL_ERROR);
        exercise(baud, UINT32_MAX / 1000U - 500U, 8, MODBUS_RESULT_OK);
    }
    exercise(1200, 0, 9, MODBUS_RESULT_OK, true);
    exercise(1200, 0, 3, MODBUS_RESULT_PROTOCOL_ERROR, true);
    puts("RS485 delayed receive tests passed");
}

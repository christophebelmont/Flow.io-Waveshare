#include <assert.h>
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpBridge.h"

namespace {
struct FakeScheduler {
    ModbusRequest last{};
    ModbusResponse reply{};
    ModbusResultCode submitResult = MODBUS_RESULT_OK;
    ModbusResultCode pollResult = MODBUS_RESULT_NOT_READY;
    uint32_t submitted = 0;
    uint32_t canceled = 0;
    static ModbusResultCode submit(void* ctx, const ModbusRequest* req, uint16_t* id) {
        auto& self = *static_cast<FakeScheduler*>(ctx);
        self.last = *req;
        ++self.submitted;
        if (self.submitResult == MODBUS_RESULT_OK) *id = 42;
        return self.submitResult;
    }
    static ModbusResultCode poll(void* ctx, uint16_t id, ModbusResponse* out) {
        auto& self = *static_cast<FakeScheduler*>(ctx);
        assert(id == 42);
        if (self.pollResult == MODBUS_RESULT_OK) *out = self.reply;
        return self.pollResult;
    }
    static void cancel(void* ctx, uint8_t) {
        ++static_cast<FakeScheduler*>(ctx)->canceled;
    }
};
}

int main() {
    using namespace ModbusTcpServer;
    FakeScheduler fake{};
    ModbusMasterService service{&FakeScheduler::submit, &FakeScheduler::poll,
                                 &FakeScheduler::cancel, nullptr, &fake};
    const uint8_t frame[] = {0x12,0x34,0,0,0,6,17,3,1,0xF4,0,24};
    ModbusTcpAdu::Request tcp{};
    assert(ModbusTcpAdu::decode(frame, sizeof(frame), tcp));
    SerialLineProfile serial{};
    BridgeSlot slot(201);
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 8000, 100));
    assert(fake.last.registerCount == 24 && fake.last.registerAddress == 500);
    assert(fake.last.responseTimeoutMs == 5000 && fake.last.transactionBudgetMs == 8000);
    slot.poll(101);
    assert(slot.state() == BridgeSlot::State::Pending);
    fake.reply.result = MODBUS_RESULT_OK;
    fake.reply.registerCount = 24;
    fake.reply.queueWaitMs = 34;
    fake.reply.rtuResponseMs = 86;
    for (uint16_t i = 0; i < 24; ++i) fake.reply.values[i] = i + 100;
    fake.pollResult = MODBUS_RESULT_OK;
    slot.poll(102);
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.hasSerialTiming());
    assert(slot.queueWaitMs() == 34 && slot.rtuResponseMs() == 86);
    assert(slot.outputSize() == 57);
    assert(slot.output()[0] == 0x12 && slot.output()[1] == 0x34);
    assert(slot.output()[7] == 3 && slot.output()[8] == 48);
    slot.reset();
    // A device-bound port redirects the RTU request without modifying TCP MBAP.
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 8000, 105, 7));
    assert(fake.last.slaveAddress == 7 && fake.last.registerAddress == 500);
    slot.poll(106);
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.output()[6] == 17);
    slot.reset();
    fake.pollResult = MODBUS_RESULT_NOT_READY;
    assert(slot.submit(service, tcp, serial, ErrorMode::Silent, 300, 200));
    slot.poll(501);
    assert(slot.state() == BridgeSlot::State::Silent);
    assert(slot.outputSize() == 0 && fake.canceled == 1);
    slot.reset();
    fake.submitResult = MODBUS_RESULT_QUEUE_FULL;
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 300, 600));
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.outputSize() == 9 && slot.output()[7] == 0x83 && slot.output()[8] == 0x06);
    assert(slot.resultCode() == MODBUS_RESULT_QUEUE_FULL);
    slot.reset();
    fake.submitResult = MODBUS_RESULT_OK;
    fake.pollResult = MODBUS_RESULT_OK;
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 100, 700));
    slot.poll(800);  // Even a ready RTU response is too late after deadline.
    assert(slot.resultCode() == MODBUS_RESULT_TIMEOUT);
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.output()[7] == 0x83 && slot.output()[8] == 0x0B);
    slot.reset();
    ModbusTcpAdu::Request unsupported{};
    unsupported.transactionId = 0x77;
    unsupported.unitId = 17;
    unsupported.function = 1;
    assert(slot.rejectException(unsupported, ErrorMode::ModbusException, 0x01, 999));
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.outputSize() == 9);
    assert(slot.output()[0] == 0 && slot.output()[1] == 0x77);
    assert(slot.output()[7] == 0x81 && slot.output()[8] == 0x01);
    slot.reset();

    // A single 125-register FC03 read must be forwarded without splitting
    // into 32-register transactions, and return a complete 259-byte TCP ADU.
    const uint8_t maxRead[] = {0,0x45,0,0,0,6,17,3,0,52,0,125};
    assert(ModbusTcpAdu::decode(maxRead, sizeof(maxRead), tcp));
    fake.submitResult = MODBUS_RESULT_OK;
    fake.pollResult = MODBUS_RESULT_NOT_READY;
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 3000, 1000));
    assert(fake.last.registerAddress == 52 && fake.last.registerCount == 125);
    fake.reply = ModbusResponse{};
    fake.reply.result = MODBUS_RESULT_OK;
    fake.reply.registerCount = 125;
    for (uint16_t i = 0; i < 125; ++i) fake.reply.values[i] = i;
    fake.pollResult = MODBUS_RESULT_OK;
    slot.poll(1001);
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.outputSize() == 259);
    assert(slot.output()[4] == 0 && slot.output()[5] == 253);
    assert(slot.output()[7] == 3 && slot.output()[8] == 250);
    assert(slot.output()[257] == 0 && slot.output()[258] == 124);
    slot.reset();

    // FC16 can write the maximum 123 registers in one serial transaction.
    uint8_t writeMax[259]{};
    writeMax[5] = 253;
    writeMax[6] = 17;
    writeMax[7] = 0x10;
    writeMax[9] = 52;
    writeMax[11] = 123;
    writeMax[12] = 246;
    for (uint16_t i = 0; i < 123; ++i) {
        writeMax[13 + 2 * i] = uint8_t(i >> 8);
        writeMax[14 + 2 * i] = uint8_t(i);
    }
    assert(ModbusTcpAdu::decode(writeMax, sizeof(writeMax), tcp));
    fake.pollResult = MODBUS_RESULT_NOT_READY;
    assert(slot.submit(service, tcp, serial, ErrorMode::ModbusException, 3000, 1100));
    assert(fake.last.function == 0x10 && fake.last.registerCount == 123);
    assert(fake.last.values[0] == 0 && fake.last.values[122] == 122);
    fake.reply = ModbusResponse{};
    fake.reply.result = MODBUS_RESULT_OK;
    fake.reply.registerAddress = 52;
    fake.reply.registerCount = 123;
    fake.pollResult = MODBUS_RESULT_OK;
    slot.poll(1101);
    assert(slot.state() == BridgeSlot::State::Ready);
    assert(slot.outputSize() == 12 && slot.output()[7] == 0x10);
    assert(slot.output()[10] == 0 && slot.output()[11] == 123);
    slot.reset();
}

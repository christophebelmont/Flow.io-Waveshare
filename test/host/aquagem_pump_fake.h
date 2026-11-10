#pragma once
#include <cassert>
#include <vector>
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"

extern uint32_t testNowMs;

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

#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
#include "Modules/IOModule/IOScheduler/Rs485TransactionScheduler.h"
#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"
uint32_t testNowMs = 0;
class Transport : public IRs485Transport {
public:
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint32_t> bauds;
    bool active = false, response = false;
    unsigned aborts = 0;
    bool ready() const override { return true; }
    bool quiet(uint32_t) const override { return !active; }
    bool configureLine(const SerialLineProfile& line) override { assert(!active); bauds.push_back(line.baud); return true; }
    void tick(uint32_t) override {}
    bool startTransmit(const uint8_t* data, size_t n, uint32_t) override {
        assert(!active); active=true; sent.emplace_back(data,data+n); return true;
    }
    bool frameAvailable() const override { return response; }
    bool takeFrame(uint8_t* data, size_t capacity, size_t& n) override {
        assert(response && active); assert(capacity>=8); n=sent.back().size();
        std::memcpy(data,sent.back().data(),n); response=active=false; return true;
    }
    void abort() override { ++aborts; active=response=false; }
};
void tick(Rs485TransactionScheduler& s,uint32_t ms) { testNowMs=ms; s.tick(ms,ms*1000); }
ModbusRequest request(uint8_t owner, uint32_t baud, uint8_t priority=MODBUS_PRIORITY_COMMAND) {
    ModbusRequest r; r.ownerId=owner; r.line.baud=baud; r.function=6; r.priority=priority;
    r.responseTimeoutMs=100; r.line.quietMs=5; r.line.lateResponseGuardMs=80; return r;
}
int main() {
    Transport transport; Rs485TransactionScheduler scheduler(transport); assert(scheduler.begin());
    auto& svc=scheduler.service(); uint16_t a=0,b=0,c=0;
    auto r=request(1,9600);assert(svc.submit(svc.ctx,&r,&a)==MODBUS_RESULT_OK);tick(scheduler,1);
    assert(transport.sent.size()==1);
    auto stop=request(2,19200,MODBUS_PRIORITY_SAFETY);assert(svc.submit(svc.ctx,&stop,&b)==MODBUS_RESULT_OK);
    svc.cancelOwner(svc.ctx,1); assert(transport.aborts==0 && transport.active);
    tick(scheduler,2);assert(transport.sent.size()==1); // no preemption in the middle of the frame
    transport.response=true;tick(scheduler,3);tick(scheduler,87);assert(transport.sent.size()==1);
    tick(scheduler,88);assert(transport.sent.size()==2 && transport.bauds.back()==19200);
    transport.response=true;tick(scheduler,89);ModbusResponse result;
    assert(svc.poll(svc.ctx,b,&result)==MODBUS_RESULT_OK && result.result==MODBUS_RESULT_OK);
    assert(svc.poll(svc.ctx,a,&result)==MODBUS_RESULT_UNKNOWN_TRANSACTION);
    // Failed attempt yields the bus; a queued safety command precedes the retry.
    testNowMs=100;r=request(1,9600);assert(svc.submit(svc.ctx,&r,&a)==MODBUS_RESULT_OK);tick(scheduler,100);
    assert(svc.submit(svc.ctx,&stop,&b)==MODBUS_RESULT_OK);tick(scheduler,200);
    assert(transport.aborts==1);tick(scheduler,284);assert(transport.sent.size()==3);
    tick(scheduler,285);assert(transport.bauds.back()==19200);
    transport.response=true;tick(scheduler,286);tick(scheduler,291);assert(transport.bauds.back()==9600);
    transport.response=true;tick(scheduler,292);
    assert(svc.poll(svc.ctx,a,&result)==MODBUS_RESULT_OK && result.result==MODBUS_RESULT_OK);
    assert(svc.poll(svc.ctx,b,&result)==MODBUS_RESULT_OK);
    // A background request ages above command traffic, but never above safety.
    testNowMs=300;auto bg=request(3,4800,MODBUS_PRIORITY_BACKGROUND);assert(svc.submit(svc.ctx,&bg,&a)==MODBUS_RESULT_OK);
    testNowMs=2400;assert(svc.submit(svc.ctx,&r,&b)==MODBUS_RESULT_OK);assert(svc.submit(svc.ctx,&stop,&c)==MODBUS_RESULT_OK);
    tick(scheduler,2400);assert(transport.bauds.back()==19200);transport.response=true;tick(scheduler,2401);
    tick(scheduler,2406);assert(transport.bauds.back()==4800);
    // Reject unsupported buses and malformed wire descriptors before queueing.
    auto bad=request(1,9600);bad.line.busId=1;assert(svc.submit(svc.ctx,&bad,&a)==MODBUS_RESULT_INVALID_ARGUMENT);
    bad.line.busId=0;bad.function=0xC3;assert(svc.submit(svc.ctx,&bad,&a)==MODBUS_RESULT_INVALID_ARGUMENT);
    puts("RS485 scheduler tests passed");
}

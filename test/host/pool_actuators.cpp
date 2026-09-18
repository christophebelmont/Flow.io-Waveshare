#include <cassert>
#include <cmath>
#include <vector>
#include <tuple>
#include <cstdio>
#include "Modules/PoolDeviceModule/Drivers/PoolDeviceDriver.h"
#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"

struct FakeIo {
    std::vector<std::tuple<IoId,bool,uint32_t>> writes;
    bool values[16]{};
    bool failOff = false;
    float analog = 0;
    IOServiceV2 service{};
    FakeIo() {
        service.ctx = this;
        service.writeDigital = [](void* ctx, IoId id, uint8_t on, uint32_t time, uint8_t owner) {
            assert(owner == 1); auto& self = *static_cast<FakeIo*>(ctx);
            self.writes.emplace_back(id, bool(on), time);
            if (!on && self.failOff) return IO_ERR_HW;
            self.values[id] = on; return IO_OK;
        };
        service.readDigital = [](void* ctx, IoId id, uint8_t* on, uint32_t*, IoSeq*) {
            *on = static_cast<FakeIo*>(ctx)->values[id]; return IO_OK;
        };
        service.writeAnalog = [](void* ctx, IoId, float value, uint32_t, uint8_t owner) {
            assert(owner == 1); static_cast<FakeIo*>(ctx)->analog = value; return IO_OK;
        };
    }
};
struct FakeBus {
    ModbusMasterService service{};
    std::vector<ModbusRequest> requests;
    uint16_t next = 0, active = 0;
    bool ready = false;
    ModbusResponse response{};
    unsigned cancellations = 0;
    FakeBus() {
        service.ctx = this;
        service.submit = [](void* ctx, const ModbusRequest* r, uint16_t* id) {
            auto& self = *static_cast<FakeBus*>(ctx); self.requests.push_back(*r);
            *id = self.active = ++self.next; self.ready = false; return MODBUS_RESULT_OK;
        };
        service.cancelOwner = [](void* ctx, uint8_t) {
            auto& self = *static_cast<FakeBus*>(ctx); ++self.cancellations; self.active = 0;
        };
        service.poll = [](void* ctx, uint16_t id, ModbusResponse* r) {
            auto& self = *static_cast<FakeBus*>(ctx);
            assert(id == self.active);
            if (!self.ready) return MODBUS_RESULT_NOT_READY;
            *r = self.response; self.active = 0; return MODBUS_RESULT_OK;
        };
    }
    void reply(uint16_t value = 0, uint8_t result = MODBUS_RESULT_OK) {
        ready = true; response = {}; response.state = MODBUS_TRANSACTION_COMPLETE;
        response.result = result; response.registerCount = 1; response.values[0] = value;
    }
};
void discreteTransitions() {
    FakeIo io; DiscreteSpeedDriver driver;
    PoolDriverConfig c; c.capabilities.kind = PoolControlKind::Discrete;
    c.capabilities.stepCount = 3; c.capabilities.steps[0] = 30; c.capabilities.steps[1] = 60; c.capabilities.steps[2] = 100;
    c.outputs[0]=0; c.outputs[1]=1; c.outputs[2]=2; c.breakBeforeMakeMs=100;
    assert(driver.begin(c, &io.service, nullptr, 1));
    driver.applyTarget({true,30},1); driver.tick(10,true);
    assert(io.writes.size() == 3); assert(!io.values[0]);
    driver.tick(109,true); assert(io.writes.size()==3);
    driver.tick(110,true); assert(io.values[0]);
    driver.applyTarget({true,100},2); driver.tick(120,true);
    assert(!io.values[0] && !io.values[2]);
    // A new target during break-before-make replaces the previous target.
    driver.applyTarget({true,60},3); driver.tick(220,true);
    assert(io.values[1] && !io.values[2]);
    assert(driver.readState().appliedRevision==3);
    io.failOff=true; driver.applyTarget({true,100},4); driver.tick(230,true);
    driver.tick(500,true); assert(!io.values[2]); assert(driver.readState().phase==PoolCommandPhase::Failed);
    io.failOff=false; driver.tick(1230,true); assert(!io.values[1]);
    driver.applyTarget({false,100},5); driver.tick(1240,true);
    assert(driver.readState().appliedRevision==5 && !driver.readState().applied.running);
    driver.applyTarget({true,30},6); const auto before=io.writes.size(); driver.tick(2000,false);
    assert(io.writes.size()==before);
    assert(!validatePoolTarget(c,{true,50}));
    c.capabilities.stepCount=255; assert(!validatePoolDriverConfig(c));
}
void relayAndAnalog() {
    FakeIo io; PoolDriverConfig c; c.outputs[0]=0;
    DigitalRelayDriver relay; assert(relay.begin(c,&io.service,nullptr,1));
    relay.applyTarget({true,100},1); relay.tick(1,true); assert(io.values[0]);
    const auto n=io.writes.size(); relay.tick(2,true); assert(n==io.writes.size());
    assert(relay.readState().quality==PoolFeedbackQuality::Estimated);
    AnalogSetpointDriver analog; c.capabilities.kind=PoolControlKind::Analog;
    c.analogGain=.1f; c.analogOffset=0; c.analogOff=0;
    assert(analog.begin(c,&io.service,nullptr,1)); analog.applyTarget({true,65},1); analog.tick(3,true);
    assert(std::abs(io.analog-6.5)<.001); analog.applyTarget({false,65},2); analog.tick(4,true); assert(io.analog==0);
    assert(!validatePoolTarget(c,{true,NAN}));
    c.flowPointCount=3; c.flowPoints[0]={0,0}; c.flowPoints[1]={50,800}; c.flowPoints[2]={100,2400};
    assert(poolCalibratedFlow(c,75)==1600); assert(std::isnan(poolCalibratedFlow(c,101)));
}
void serialCommands() {
    FakeBus bus; SerialDeviceDriver d; PoolDriverConfig c; c.capabilities.kind=PoolControlKind::Rs485;
    c.serial.line.baud=19200; assert(d.begin(c,nullptr,&bus.service,1));
    d.applyTarget({true,40},1); d.tick(10,true);
    assert(bus.requests.back().registerAddress==1 && bus.requests.back().values[0]==40);
    // Replace a pending setpoint; its eventual reply must not confirm the new value.
    d.applyTarget({true,80},2); assert(bus.cancellations==1); d.tick(20,true);
    assert(bus.requests.back().values[0]==80 && !d.readState().appliedValid);
    bus.reply(); d.tick(21,true); assert(bus.requests.back().registerAddress==0);
    bus.reply(); d.tick(22,true); assert(d.readState().appliedRevision==2 && d.readState().applied.setpoint==80);
    d.applyTarget({false,80},3); d.tick(30,true);
    assert(bus.requests.back().priority==MODBUS_PRIORITY_SAFETY && bus.requests.back().values[0]==0);
    bus.reply(); d.tick(31,true); assert(!d.readState().applied.running);
    d.applyTarget({true,50},4); d.tick(40,false); const auto n=bus.requests.size(); d.tick(41,false);
    assert(bus.requests.size()==n); d.tick(42,true); assert(bus.requests.back().values[0]==50);
    bus.reply(0,MODBUS_RESULT_TIMEOUT); d.tick(43,true); assert(!d.readState().observedValid);
}
void codecDialects() {
    ModbusRequest r; r.slaveAddress=7; r.function=0xC3; r.registerCount=1;
    uint8_t frame[64]{}; size_t length=0;
    assert(!ModbusRtuCodec::encodeRequest(r,frame,sizeof(frame),length));
    r.protocol=RegisterWireProtocol::VendorRegisterRtu; r.operation=RegisterOperation::Read;
    assert(ModbusRtuCodec::encodeRequest(r,frame,sizeof(frame),length)); assert(length==8 && frame[1]==0xC3);
    uint8_t response[]={7,0xC3,2,0,75,0,0}; auto crc=ModbusRtuCodec::crc16(response,5);
    response[5]=crc;response[6]=crc>>8;
    ModbusResponse result;
    assert(ModbusRtuCodec::decodeResponse(r,response,sizeof(response),result)==MODBUS_RESULT_OK && result.values[0]==75);
    r.operation=RegisterOperation::WriteSingle;r.function=0xD0;r.values[0]=90;
    assert(ModbusRtuCodec::encodeRequest(r,frame,sizeof(frame),length));
    assert(ModbusRtuCodec::decodeResponse(r,frame,length,result)==MODBUS_RESULT_OK);
    frame[3]^=1;assert(ModbusRtuCodec::decodeResponse(r,frame,length,result)==MODBUS_RESULT_CRC_ERROR);
    r.protocol=RegisterWireProtocol::ModbusRtu;r.function=3;
    uint8_t exception[]={7,0x83,2,0,0};crc=ModbusRtuCodec::crc16(exception,3);exception[3]=crc;exception[4]=crc>>8;
    assert(ModbusRtuCodec::decodeResponse(r,exception,sizeof(exception),result)==MODBUS_RESULT_EXCEPTION);
}
int main() { discreteTransitions();relayAndAnalog();serialCommands();codecDialects();puts("pool actuator tests passed"); }

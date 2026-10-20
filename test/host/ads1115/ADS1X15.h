#pragma once
#include "Core/I2cBus.h"
#define ADS1X15_GAIN_6144MV 0
#define ADS1X15_OK 0
#define ADS1X15_INVALID_VOLTAGE -9999
struct AdsFake {
    bool requestOk = true;
    bool statusOk = true;
    bool readOk = true;
    bool ready = true;
    unsigned requests = 0;
    uint8_t channel = 0;
};
inline AdsFake adsFake;
class ADS1115 {
    int8_t error_ = 0;
public:
    ADS1115(uint8_t, TwoWire*) {}
    bool begin() { return true; }
    bool isConnected() { return true; }
    void setGain(uint8_t) {}
    void setDataRate(uint8_t) {}
    int8_t getError() { int8_t e = error_; error_ = 0; return e; }
    void requestADC(uint8_t channel) {
        ++adsFake.requests;
        adsFake.channel = channel;
        if (!adsFake.requestOk) error_ = -1;
    }
    void requestADC_Differential_0_1() { requestADC(0); }
    void requestADC_Differential_2_3() { requestADC(1); }
    bool isReady() { if (!adsFake.statusOk) error_ = -1; return adsFake.ready; }
    int16_t getValue() { if (!adsFake.readOk) error_ = -1; return 100 + adsFake.channel; }
    float toVoltage(int16_t raw) { return raw * 0.001f; }
};

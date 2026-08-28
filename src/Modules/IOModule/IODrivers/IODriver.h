#pragma once
/**
 * @file IODriver.h
 * @brief Base interface for IO drivers.
 */

#include <stdint.h>

class IODriver {
public:
    virtual ~IODriver() = default;
    virtual const char* id() const = 0;
    virtual bool begin() = 0;
    virtual void tick(uint32_t nowMs) = 0;
};

struct IOAnalogSample {
    float value = 0.0f;
    int16_t raw = 0;
    uint32_t seq = 0;
    bool hasRaw = false;
    bool hasSeq = false;
};

class IDigitalPinDriver : public IODriver {
public:
    virtual bool write(bool on) = 0;
    virtual bool read(bool& on) const = 0;
};

struct IODigitalCounterDebugStats {
    uint8_t pin = 0;
    uint8_t edgeMode = 0;
    bool activeHigh = true;
    bool logicalState = false;
    int32_t pulseCount = 0;
    uint32_t irqCalls = 0;
    uint32_t transitions = 0;
    uint32_t ignoredSameState = 0;
    uint32_t ignoredWrongEdge = 0;
    uint32_t ignoredDebounce = 0;
    uint32_t lastPulseUs = 0;
};

class IDigitalCounterDriver : public IDigitalPinDriver {
public:
    virtual bool readCount(int32_t& count) const = 0;
    virtual bool readDebugStats(IODigitalCounterDebugStats& out) const
    {
        (void)out;
        return false;
    }
};

class IAnalogSourceDriver : public IODriver {
public:
    virtual bool readSample(uint8_t channel, IOAnalogSample& out) const = 0;
};

class IMaskOutputDriver : public IODriver {
public:
    virtual bool writeMask(uint8_t mask) = 0;
    virtual bool readMask(uint8_t& mask) const = 0;
};

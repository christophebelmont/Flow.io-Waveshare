#pragma once
/**
 * @file IOProviders.h
 * @brief Tiny non-owning provider views used by the IO kernel.
 *
 * These structs deliberately store only a context pointer and a few function
 * pointers. They do not allocate, do not own the concrete driver, and avoid
 * duplicating endpoint state. The kernel sees stable provider capabilities
 * while product assembly still decides which concrete driver is instantiated.
 */

#include <stdint.h>

#include "Modules/IOModule/IODrivers/IODriver.h"

typedef bool (*IOProviderBeginFn)(void* ctx);
typedef void (*IOProviderTickFn)(void* ctx, uint32_t nowMs);
typedef bool (*IOProviderReadAnalogFn)(void* ctx, uint8_t channel, IOAnalogSample& out);
typedef bool (*IOProviderReadDigitalFn)(void* ctx, bool& outOn);
typedef bool (*IOProviderReadCounterFn)(void* ctx, int32_t& outCount);
typedef bool (*IOProviderWriteDigitalFn)(void* ctx, bool on);
typedef bool (*IOProviderReadMaskFn)(void* ctx, uint8_t& outMask);
typedef bool (*IOProviderWriteMaskFn)(void* ctx, uint8_t mask);

struct IOAnalogProvider {
    void* ctx = nullptr;
    IOProviderBeginFn beginFn = nullptr;
    IOProviderTickFn tickFn = nullptr;
    IOProviderReadAnalogFn readFn = nullptr;

    bool isBound() const { return ctx != nullptr && readFn != nullptr; }
    bool begin() const { return beginFn ? beginFn(ctx) : false; }
    void tick(uint32_t nowMs) const { if (tickFn) tickFn(ctx, nowMs); }
    bool readSample(uint8_t channel, IOAnalogSample& out) const
    {
        return readFn ? readFn(ctx, channel, out) : false;
    }
};

struct IODigitalProvider {
    void* ctx = nullptr;
    IOProviderBeginFn beginFn = nullptr;
    IOProviderReadDigitalFn readFn = nullptr;
    IOProviderWriteDigitalFn writeFn = nullptr;

    bool isBound() const { return ctx != nullptr; }
    bool begin() const { return beginFn ? beginFn(ctx) : false; }
    bool read(bool& outOn) const { return readFn ? readFn(ctx, outOn) : false; }
    bool write(bool on) const { return writeFn ? writeFn(ctx, on) : false; }
};

struct IOCounterProvider {
    void* ctx = nullptr;
    IOProviderBeginFn beginFn = nullptr;
    IOProviderReadCounterFn readFn = nullptr;

    bool isBound() const { return ctx != nullptr && readFn != nullptr; }
    bool begin() const { return beginFn ? beginFn(ctx) : false; }
    bool readCount(int32_t& outCount) const { return readFn ? readFn(ctx, outCount) : false; }
};

struct IOMaskProvider {
    void* ctx = nullptr;
    IOProviderBeginFn beginFn = nullptr;
    IOProviderReadMaskFn readFn = nullptr;
    IOProviderWriteMaskFn writeFn = nullptr;

    bool isBound() const { return ctx != nullptr; }
    bool begin() const { return beginFn ? beginFn(ctx) : false; }
    bool readMask(uint8_t& outMask) const { return readFn ? readFn(ctx, outMask) : false; }
    bool writeMask(uint8_t mask) const { return writeFn ? writeFn(ctx, mask) : false; }
};

inline bool ioProviderBeginFromDriver_(void* ctx)
{
    return ctx ? static_cast<IODriver*>(ctx)->begin() : false;
}

inline void ioProviderTickFromDriver_(void* ctx, uint32_t nowMs)
{
    if (ctx) static_cast<IODriver*>(ctx)->tick(nowMs);
}

inline bool ioProviderReadAnalogFromDriver_(void* ctx, uint8_t channel, IOAnalogSample& out)
{
    return ctx ? static_cast<IAnalogSourceDriver*>(ctx)->readSample(channel, out) : false;
}

inline bool ioProviderReadDigitalFromDriver_(void* ctx, bool& outOn)
{
    return ctx ? static_cast<IDigitalPinDriver*>(ctx)->read(outOn) : false;
}

inline bool ioProviderWriteDigitalFromDriver_(void* ctx, bool on)
{
    return ctx ? static_cast<IDigitalPinDriver*>(ctx)->write(on) : false;
}

inline bool ioProviderReadCounterFromDriver_(void* ctx, int32_t& outCount)
{
    return ctx ? static_cast<IDigitalCounterDriver*>(ctx)->readCount(outCount) : false;
}

inline bool ioProviderReadMaskFromDriver_(void* ctx, uint8_t& outMask)
{
    return ctx ? static_cast<IMaskOutputDriver*>(ctx)->readMask(outMask) : false;
}

inline bool ioProviderWriteMaskFromDriver_(void* ctx, uint8_t mask)
{
    return ctx ? static_cast<IMaskOutputDriver*>(ctx)->writeMask(mask) : false;
}

inline IOAnalogProvider makeAnalogProvider(IAnalogSourceDriver* driver)
{
    IOAnalogProvider provider{};
    provider.ctx = driver;
    provider.beginFn = ioProviderBeginFromDriver_;
    provider.tickFn = ioProviderTickFromDriver_;
    provider.readFn = ioProviderReadAnalogFromDriver_;
    return provider;
}

inline IODigitalProvider makeDigitalProvider(IDigitalPinDriver* driver)
{
    IODigitalProvider provider{};
    provider.ctx = driver;
    provider.beginFn = ioProviderBeginFromDriver_;
    provider.readFn = ioProviderReadDigitalFromDriver_;
    provider.writeFn = ioProviderWriteDigitalFromDriver_;
    return provider;
}

inline IOCounterProvider makeCounterProvider(IDigitalCounterDriver* driver)
{
    IOCounterProvider provider{};
    provider.ctx = driver;
    provider.beginFn = ioProviderBeginFromDriver_;
    provider.readFn = ioProviderReadCounterFromDriver_;
    return provider;
}

inline IOMaskProvider makeMaskProvider(IMaskOutputDriver* driver)
{
    IOMaskProvider provider{};
    provider.ctx = driver;
    provider.beginFn = ioProviderBeginFromDriver_;
    provider.readFn = ioProviderReadMaskFromDriver_;
    provider.writeFn = ioProviderWriteMaskFromDriver_;
    return provider;
}

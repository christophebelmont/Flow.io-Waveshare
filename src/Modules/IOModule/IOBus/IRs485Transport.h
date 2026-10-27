#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Core/Services/IModbusMaster.h"

constexpr size_t RS485_MAX_FRAME_BYTES = 256;
/** Single-owner transport contract; the scheduler is its only runtime caller. */
class IRs485Transport {
public:
    virtual ~IRs485Transport() = default;
    virtual bool ready() const = 0;
    virtual bool configureLine(const SerialLineProfile&) = 0;
    virtual bool quiet(uint32_t nowUs) const = 0;
    virtual void tick(uint32_t nowUs) = 0;
    virtual bool startTransmit(const uint8_t*, size_t length, uint32_t nowUs) = 0;
    virtual bool frameAvailable() const = 0;
    // Buffered bytes awaiting the inter-frame silence. tick() must either
    // publish the frame after that silence or abort on receive overflow.
    virtual bool receivingFrame() const = 0;
    virtual bool takeFrame(uint8_t*, size_t capacity, size_t& length) = 0;
    virtual void abort() = 0;
};

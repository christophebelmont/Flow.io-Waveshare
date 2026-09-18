#pragma once
/**
 * @file Rs485Bus.h
 * @brief Non-blocking half-duplex RS485 transport over one hardware UART.
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <stddef.h>
#include <stdint.h>

#include "Board/BoardTypes.h"
#include "IRs485Transport.h"


struct Rs485BusStats {
    uint32_t transmittedFrames = 0U;
    uint32_t receivedFrames = 0U;
    uint32_t receiveOverflows = 0U;
    uint32_t ioErrors = 0U;
};

class Rs485Bus final : public IRs485Transport {
public:
    explicit Rs485Bus(HardwareSerial& serial) : serial_(serial) {}

    bool begin(const UartSpec& spec);
    void end();
    bool configureLine(const SerialLineProfile& profile);
    bool quiet(uint32_t nowUs) const;
    void tick(uint32_t nowUs);

    bool startTransmit(const uint8_t* frame, size_t length, uint32_t nowUs);
    bool frameAvailable() const { return frameReady_; }
    bool takeFrame(uint8_t* out, size_t capacity, size_t& outLength);
    void abort();

    bool ready() const { return ready_; }
    bool busy() const { return state_ != State::Idle; }
    const Rs485BusStats& stats() const { return stats_; }

private:
    enum class State : uint8_t {
        Idle,
        Transmitting,
        Receiving
    };

    struct Storage {
        uint8_t rxBuffer[RS485_MAX_FRAME_BYTES]{};
    };

    static bool deadlineReached_(uint32_t now, uint32_t deadline) {
        return (int32_t)(now - deadline) >= 0;
    }
    void setDirection_(bool transmit);
    static bool serialConfig_(const UartSpec& spec, uint32_t& outConfig);
    uint32_t frameDurationUs_(size_t bytes) const;

    HardwareSerial& serial_;
    UartSpec spec_{};
    State state_ = State::Idle;
    bool ready_ = false;
    bool frameReady_ = false;
    // Retained across end()/begin(); only the UART driver is stopped by end().
    Storage* storage_ = nullptr;
    bool storageAllocationAttempted_ = false;
    size_t rxLength_ = 0U;
    uint32_t txCompleteAtUs_ = 0U;
    uint32_t lastRxAtUs_ = 0U;
    uint32_t frameGapUs_ = 4000U;
    Rs485BusStats stats_{};
};

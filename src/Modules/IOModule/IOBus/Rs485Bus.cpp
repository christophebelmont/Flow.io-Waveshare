/**
 * @file Rs485Bus.cpp
 * @brief Non-blocking half-duplex RS485 transport implementation.
 */

#include "Rs485Bus.h"

#include <string.h>
#include <esp_heap_caps.h>
#include <new>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

bool Rs485Bus::begin(const UartSpec& spec)
{
    if (spec.rxPin < 0 || spec.txPin < 0 || spec.baud == 0U) return false;
    uint32_t serialConfig = SERIAL_8N1;
    if (!serialConfig_(spec, serialConfig)) return false;
    if (!storageAllocationAttempted_) {
        storageAllocationAttempted_ = true;
        void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) {
            storage_ = new (memory) Storage{};
            LOGI("RS485 storage ready bytes=%u memory=psram", (unsigned)sizeof(Storage));
        } else {
            LOGE("RS485 unavailable: PSRAM allocation failed bytes=%u", (unsigned)sizeof(Storage));
        }
    }
    if (!storage_) return false;
    spec_ = spec;
    rxLength_ = 0U;
    frameReady_ = false;
    state_ = State::Idle;
    stats_ = Rs485BusStats{};

    if (spec_.directionPin >= 0) {
        pinMode((uint8_t)spec_.directionPin, OUTPUT);
        setDirection_(false);
    }

    serial_.begin(spec_.baud, serialConfig, spec_.rxPin, spec_.txPin);
    frameGapUs_ = frameDurationUs_(4U);
    ready_ = true;
    return true;
}

bool Rs485Bus::serialConfig_(const UartSpec& spec, uint32_t& outConfig)
{
    if (spec.stopBits != 1U && spec.stopBits != 2U) return false;
    switch (spec.parity) {
        case UartSpec::Parity::None:
            outConfig = spec.stopBits == 1U ? SERIAL_8N1 : SERIAL_8N2;
            return true;
        case UartSpec::Parity::Even:
            outConfig = spec.stopBits == 1U ? SERIAL_8E1 : SERIAL_8E2;
            return true;
        case UartSpec::Parity::Odd:
            outConfig = spec.stopBits == 1U ? SERIAL_8O1 : SERIAL_8O2;
            return true;
    }
    return false;
}

void Rs485Bus::end()
{
    if (ready_) serial_.end();
    ready_ = false;
    state_ = State::Idle;
    frameReady_ = false;
    rxLength_ = 0U;
}

void Rs485Bus::setDirection_(bool transmit)
{
    if (spec_.directionPin < 0) return;
    const bool level = transmit ? spec_.directionTxHigh : !spec_.directionTxHigh;
    digitalWrite((uint8_t)spec_.directionPin, level ? HIGH : LOW);
}

uint32_t Rs485Bus::frameDurationUs_(size_t bytes) const
{
    // Start + eight data bits + optional parity + configured stop bits.
    const uint8_t bitsPerCharacter = (uint8_t)(9U + spec_.stopBits +
        (spec_.parity == UartSpec::Parity::None ? 0U : 1U));
    // One extra character covers the UART FIFO drain and DE release margin.
    const uint64_t bits = (uint64_t)(bytes + 1U) * bitsPerCharacter;
    const uint64_t duration = (bits * 1000000ULL + spec_.baud - 1U) / spec_.baud;
    return (duration > UINT32_MAX) ? UINT32_MAX : (uint32_t)duration;
}

bool Rs485Bus::startTransmit(const uint8_t* frame, size_t length, uint32_t nowUs)
{
    if (!ready_ || !frame || length == 0U || length > RS485_MAX_FRAME_BYTES) return false;
    if (state_ != State::Idle || frameReady_) return false;

    while (serial_.available() > 0) (void)serial_.read();
    rxLength_ = 0U;
    setDirection_(true);
    const size_t written = serial_.write(frame, length);
    if (written != length) {
        ++stats_.ioErrors;
        setDirection_(false);
        return false;
    }

    ++stats_.transmittedFrames;
    txCompleteAtUs_ = nowUs + frameDurationUs_(length);
    state_ = State::Transmitting;
    return true;
}

void Rs485Bus::tick(uint32_t nowUs)
{
    if (!ready_) return;
    if (state_ == State::Transmitting && deadlineReached_(nowUs, txCompleteAtUs_)) {
        setDirection_(false);
        state_ = State::Receiving;
        lastRxAtUs_ = nowUs;
    }
    if (state_ != State::Receiving || frameReady_) return;

    while (serial_.available() > 0) {
        const int byteValue = serial_.read();
        if (byteValue < 0) break;
        if (rxLength_ >= sizeof(storage_->rxBuffer)) {
            ++stats_.receiveOverflows;
            abort();
            return;
        }
        storage_->rxBuffer[rxLength_++] = (uint8_t)byteValue;
        lastRxAtUs_ = nowUs;
    }

    if (rxLength_ > 0U && (uint32_t)(nowUs - lastRxAtUs_) >= frameGapUs_) {
        frameReady_ = true;
        state_ = State::Idle;
        ++stats_.receivedFrames;
    }
}

bool Rs485Bus::takeFrame(uint8_t* out, size_t capacity, size_t& outLength)
{
    outLength = 0U;
    if (!storage_ || !frameReady_ || !out || capacity < rxLength_) return false;
    memcpy(out, storage_->rxBuffer, rxLength_);
    outLength = rxLength_;
    rxLength_ = 0U;
    frameReady_ = false;
    return true;
}

void Rs485Bus::abort()
{
    setDirection_(false);
    state_ = State::Idle;
    frameReady_ = false;
    rxLength_ = 0U;
    while (ready_ && serial_.available() > 0) (void)serial_.read();
}

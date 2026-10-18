/**
 * @file Rs485Bus.cpp
 * @brief Non-blocking half-duplex RS485 transport implementation.
 */

#include "Rs485Bus.h"

#include <string.h>
#include <esp_heap_caps.h>
#include <new>
#include <driver/uart.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

bool Rs485Bus::begin(const UartSpec& spec)
{
    LOGD("RS485 init uart=%u rx=%d tx=%d baud=%lu parity=%u stop=%u de=%d tx_high=%u",
         spec.uartIndex, spec.rxPin, spec.txPin, (unsigned long)spec.baud,
         (unsigned)spec.parity, spec.stopBits, spec.directionPin, spec.directionTxHigh);
    if (spec.rxPin < 0 || spec.txPin < 0 || spec.baud == 0U) {
        LOGD("RS485 init rejected: invalid pins or baud");
        return false;
    }
    uint32_t serialConfig = SERIAL_8N1;
    if (!serialConfig_(spec, serialConfig)) {
        LOGD("RS485 init rejected: invalid parity or stop bits");
        return false;
    }
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

    serial_.setTxBufferSize(RS485_MAX_FRAME_BYTES);
    serial_.begin(spec_.baud, serialConfig, spec_.rxPin, spec_.txPin);
    frameGapUs_ = spec_.baud > 19200 ? 1750U : frameDurationUs_(3U);
    lastRxAtUs_ = micros();
    ready_ = true;
    LOGD("RS485 initialized uart=%u frame_gap_us=%lu direction=%s",
         spec_.uartIndex, (unsigned long)frameGapUs_,
         spec_.directionPin < 0 ? "automatic" : "gpio");
    return true;
}

bool Rs485Bus::configureLine(const SerialLineProfile& p)
{
    if (!ready_ || busy() || frameReady_ || p.busId != 0 || p.baud < 1200 ||
        p.baud > 115200 || p.parity > 2 || (p.stopBits != 1 && p.stopBits != 2)) {
        LOGD("RS485 line rejected ready=%u busy=%u frame_ready=%u bus=%u baud=%lu parity=%u stop=%u",
             ready_, busy(), frameReady_, p.busId, (unsigned long)p.baud, p.parity, p.stopBits);
        return false;
    }
    const auto port = static_cast<uart_port_t>(spec_.uartIndex);
    const uart_parity_t parity[] = {UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD};
    if (uart_set_baudrate(port, p.baud) != ESP_OK ||
        uart_set_parity(port, parity[p.parity]) != ESP_OK ||
        uart_set_stop_bits(port, p.stopBits == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2) != ESP_OK) {
        LOGD("RS485 line configuration failed uart=%u", spec_.uartIndex);
        return false;
    }
    if (spec_.baud != p.baud || (unsigned)spec_.parity != p.parity || spec_.stopBits != p.stopBits) {
        LOGD("RS485 line uart=%u baud=%lu parity=%u stop=%u",
             spec_.uartIndex, (unsigned long)p.baud, p.parity, p.stopBits);
    }
    spec_.baud = p.baud;
    spec_.parity = static_cast<UartSpec::Parity>(p.parity);
    spec_.stopBits = p.stopBits;
    const uint32_t bits = 9U + (p.parity != 0) + p.stopBits;
    frameGapUs_ = p.baud > 19200 ? 1750U : (35U * bits * 100000U + p.baud - 1U) / p.baud;
    return true;
}

bool Rs485Bus::quiet(uint32_t nowUs) const
{
    return !busy() && !frameReady_ && uint32_t(nowUs - lastRxAtUs_) >= frameGapUs_;
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
    LOGD("RS485 stop uart=%u ready=%u tx=%lu rx=%lu overflows=%lu io_errors=%lu",
         spec_.uartIndex, ready_, (unsigned long)stats_.transmittedFrames,
         (unsigned long)stats_.receivedFrames, (unsigned long)stats_.receiveOverflows,
         (unsigned long)stats_.ioErrors);
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
        LOGD("RS485 TX incomplete uart=%u written=%u expected=%u",
             spec_.uartIndex, (unsigned)written, (unsigned)length);
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
    if (state_ == State::Idle && !frameReady_) {
        while (serial_.available() > 0) { (void)serial_.read(); lastRxAtUs_ = nowUs; }
    }
    if (state_ == State::Transmitting &&
        uart_wait_tx_done(static_cast<uart_port_t>(spec_.uartIndex), 0) == ESP_OK) {
        setDirection_(false);
        state_ = State::Receiving;
        lastRxAtUs_ = nowUs;
    }
    if (state_ != State::Receiving || frameReady_) return;

    while (serial_.available() > 0) {
        const int byteValue = serial_.read();
        if (byteValue < 0) break;
        if (rxLength_ >= sizeof(storage_->rxBuffer)) {
            LOGD("RS485 RX overflow uart=%u capacity=%u", spec_.uartIndex,
                 (unsigned)sizeof(storage_->rxBuffer));
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
    LOGD("RS485 abort uart=%u state=%u partial_rx_bytes=%u",
         spec_.uartIndex, (unsigned)state_, (unsigned)rxLength_);
    setDirection_(false);
    state_ = State::Idle;
    frameReady_ = false;
    rxLength_ = 0U;
    while (ready_ && serial_.available() > 0) (void)serial_.read();
    lastRxAtUs_ = micros();
}

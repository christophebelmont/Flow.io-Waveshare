#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "Board/BoardCatalog.h"
#include "Board/BoardSpec.h"

namespace Board {
namespace SerialMap {

#ifndef FLOW_SWAP_LOG_HMI_SERIAL
#define FLOW_SWAP_LOG_HMI_SERIAL 0
#endif

#ifndef FLOW_ALLOW_HMI_ON_NATIVE_USB_PINS
#define FLOW_ALLOW_HMI_ON_NATIVE_USB_PINS 0
#endif

static constexpr bool SwapLogAndHmi = (FLOW_SWAP_LOG_HMI_SERIAL != 0);
static constexpr bool AllowHmiOnNativeUsbPins = (FLOW_ALLOW_HMI_ON_NATIVE_USB_PINS != 0);

static constexpr uint32_t LogBaud = 115200;
static constexpr uint32_t HmiBaud = 115200;

static constexpr int8_t NoPin = -1;
static constexpr int8_t Uart2Rx = 16;
static constexpr int8_t Uart2Tx = 17;
static constexpr int8_t Esp32S3UsbDmPin = 19;
static constexpr int8_t Esp32S3UsbDpPin = 20;

inline bool isNativeUsbCdcConsole()
{
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1) && \
    defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 1)
    return true;
#else
    return false;
#endif
}

inline bool isNativeUsbPin(int8_t pin)
{
    return pin == Esp32S3UsbDmPin || pin == Esp32S3UsbDpPin;
}

inline const UartSpec* hmiUartSpec()
{
    return boardFindUart(BoardCatalog::activeBoard(), "hmi");
}

inline uint32_t hmiBaud()
{
    const UartSpec* spec = hmiUartSpec();
    return spec ? spec->baud : HmiBaud;
}

inline bool hmiUartAvailable()
{
    if (SwapLogAndHmi) return true;
    const UartSpec* spec = hmiUartSpec();
    if (!spec) return false;
    if (!isNativeUsbCdcConsole() || AllowHmiOnNativeUsbPins) return true;
    return !isNativeUsbPin(spec->rxPin) && !isNativeUsbPin(spec->txPin);
}

inline bool hmiUartBlockedByNativeUsb()
{
    if (SwapLogAndHmi || !isNativeUsbCdcConsole() || AllowHmiOnNativeUsbPins) return false;
    const UartSpec* spec = hmiUartSpec();
    return spec && (isNativeUsbPin(spec->rxPin) || isNativeUsbPin(spec->txPin));
}

inline Stream& logSerial()
{
    return SwapLogAndHmi ? static_cast<Stream&>(Serial2) : static_cast<Stream&>(Serial);
}

inline void beginLogSerial()
{
    if (SwapLogAndHmi) {
        const UartSpec* spec = hmiUartSpec();
        const int8_t rx = spec ? spec->rxPin : Uart2Rx;
        const int8_t tx = spec ? spec->txPin : Uart2Tx;
        Serial2.begin(LogBaud, SERIAL_8N1, rx, tx);
    } else {
        Serial.begin(LogBaud);
    }
}

inline HardwareSerial& hmiSerial()
{
    if constexpr (SwapLogAndHmi) {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
        return Serial2;
#else
        return Serial;
#endif
    } else {
        return Serial2;
    }
}

inline int8_t logRxPin()
{
    return SwapLogAndHmi ? Uart2Rx : NoPin;
}

inline int8_t logTxPin()
{
    return SwapLogAndHmi ? Uart2Tx : NoPin;
}

inline int8_t hmiRxPin()
{
    if (SwapLogAndHmi) return NoPin;
    if (!hmiUartAvailable()) return NoPin;
    const UartSpec* spec = hmiUartSpec();
    return spec ? spec->rxPin : NoPin;
}

inline int8_t hmiTxPin()
{
    if (SwapLogAndHmi) return NoPin;
    if (!hmiUartAvailable()) return NoPin;
    const UartSpec* spec = hmiUartSpec();
    return spec ? spec->txPin : NoPin;
}

inline uint32_t uart0Baud()
{
    return SwapLogAndHmi ? HmiBaud : LogBaud;
}

}  // namespace SerialMap
}  // namespace Board

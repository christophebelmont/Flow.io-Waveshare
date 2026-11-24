# Changelog — Nextion OTA Reliability Improvements

**Date:** November 24, 2026

## Improvements

- Download the complete Nextion `.tft` firmware to PSRAM before flashing, with HTTP retries and mandatory SHA-256 verification.
- Fix UART conflicts by using **Serial2** for Nextion instead of the Modbus RS485 port.
- Improve serial transfer reliability, including final-block acknowledgements and watchdog prevention.
- Give OTA updates priority over Modbus TCP/RTU traffic.
- Prevent simultaneous Nextion and ESP32 firmware + SPIFFS updates.
- Display actual download and flashing progress in the web interface (FR/EN).

## Validation

- Nextion firmware update successfully tested on hardware.
- **4,495 simulated scenarios** and **44 static checks** passed.
- Additional regression testing and review by Christophe are recommended.

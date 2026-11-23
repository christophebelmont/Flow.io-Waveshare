#pragma once

#include "../stubs/Arduino.h"

#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>

constexpr uint32_t SERIAL_8N1 = 0;
inline void delay(uint32_t ms) { testNowMs += ms; }

class HardwareSerial {
public:
    std::deque<uint8_t> received;
    std::vector<uint8_t> transmitted;

    void begin(uint32_t, uint32_t = SERIAL_8N1, int8_t = -1, int8_t = -1) {}
    void end() {}
    int available() const { return static_cast<int>(received.size()); }
    int read() {
        if (received.empty()) return -1;
        const int value = received.front();
        received.pop_front();
        return value;
    }
    size_t print(const char* text) {
        const size_t length = std::strlen(text);
        transmitted.insert(transmitted.end(), text, text + length);
        return length;
    }
    size_t write(uint8_t value) {
        transmitted.push_back(value);
        return 1;
    }
};

inline HardwareSerial Serial2;

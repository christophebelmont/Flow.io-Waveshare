#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
class HardwareSerial {
public:
    std::deque<uint8_t> received;
    std::vector<uint8_t> transmitted;
    void setTxBufferSize(size_t) {}
    void begin(uint32_t, uint32_t, int8_t, int8_t) {}
    void end() {}
    int available() const { return static_cast<int>(received.size()); }
    int read() {
        if (received.empty()) return -1;
        const int value = received.front(); received.pop_front(); return value;
    }
    size_t write(const uint8_t* data, size_t length) {
        transmitted.assign(data, data + length); return length;
    }
};

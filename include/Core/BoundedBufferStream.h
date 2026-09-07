#pragma once
/**
 * @file BoundedBufferStream.h
 * @brief Write-only Arduino stream backed by a fixed-size caller-owned buffer.
 */

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

class BoundedBufferStream final : public Stream {
public:
    BoundedBufferStream(void* data, size_t capacity)
        : data_(static_cast<uint8_t*>(data)), capacity_(data ? capacity : 0U)
    {
    }

    size_t write(uint8_t value) override
    {
        return write(&value, 1U);
    }

    size_t write(const uint8_t* data, size_t len) override
    {
        if (!data || len == 0U) return 0U;
        const size_t room = length_ < capacity_ ? capacity_ - length_ : 0U;
        const size_t count = len < room ? len : room;
        if (count > 0U && data_) {
            memcpy(data_ + length_, data, count);
            length_ += count;
        }
        if (count != len) {
            overflowed_ = true;
            setWriteError();
        }
        return count;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    size_t length() const { return length_; }
    bool overflowed() const { return overflowed_; }

private:
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0U;
    size_t length_ = 0U;
    bool overflowed_ = false;
};

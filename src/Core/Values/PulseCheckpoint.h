#pragma once
#include <stdint.h>
#include <stddef.h>

namespace PulseCheckpoint {
constexpr uint8_t Capacity = 16;
constexpr uint32_t PeriodMs = 3600000;
constexpr uint32_t RetryMs = 60000;
constexpr size_t Size = 8 + Capacity * 14 + 4;
constexpr const char* Key = "pulse_v1";
struct State { uint64_t count[Capacity]{}; uint32_t generation[Capacity]{}; uint16_t resetToken[Capacity]{}; };
inline uint32_t checksum(const uint8_t* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
inline void put(uint8_t*& p, uint64_t v, uint8_t length) {
    for (uint8_t i = 0; i < length; ++i) { *p++ = static_cast<uint8_t>(v); v >>= 8; }
}
inline uint64_t get(const uint8_t*& p, uint8_t length) {
    uint64_t v = 0;
    for (uint8_t i = 0; i < length; ++i) v |= uint64_t(*p++) << (8 * i);
    return v;
}
inline void encode(const State& state, uint8_t (&out)[Size]) {
    auto* p = out; put(p, 0x31534c50u, 4); put(p, 1, 4);
    for (uint8_t i = 0; i < Capacity; ++i) { put(p, state.count[i], 8); put(p, state.generation[i], 4); put(p, state.resetToken[i], 2); }
    put(p, checksum(out, Size - 4), 4);
}
inline bool decode(const uint8_t* bytes, size_t size, State& out) {
    if (size != Size) return false;
    auto* p = bytes;
    if (get(p, 4) != 0x31534c50u || get(p, 4) != 1) return false;
    auto* crc = bytes + Size - 4;
    if (get(crc, 4) != checksum(bytes, Size - 4)) return false;
    for (uint8_t i = 0; i < Capacity; ++i) { out.count[i] = get(p, 8); out.generation[i] = get(p, 4); out.resetToken[i] = get(p, 2); }
    return true;
}
}

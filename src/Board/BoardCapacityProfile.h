#pragma once

#include "Board/WaveshareBoard.h"

namespace BoardCapacityProfile {

inline constexpr IoCapacitySpec kIoCapacity = BoardProfiles::kWaveshareESP32S3.ioCapacity;
inline constexpr MqttCapacitySpec kMqttCapacity = BoardProfiles::kWaveshareESP32S3.mqttCapacity;
inline constexpr MqttBufferSpec kMqttBuffers = BoardProfiles::kWaveshareESP32S3.mqttBuffers;
inline constexpr HaCapacitySpec kHaCapacity = BoardProfiles::kWaveshareESP32S3.haCapacity;

}  // namespace BoardCapacityProfile

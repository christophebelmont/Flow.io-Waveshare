#pragma once
namespace VolumeUnits {
constexpr float millilitresToLitres(float value) { return value / 1000.0f; }
constexpr float litresToMillilitres(float value) { return value * 1000.0f; }
}

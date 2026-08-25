#pragma once

#ifndef FIRMW
#define FIRMW "unknown"
#endif

#ifndef FLOW_BUILD_REF
#define FLOW_BUILD_REF "dev"
#endif

#ifndef FLOW_FIRMWARE_VERSION_FULL
#define FLOW_FIRMWARE_VERSION_FULL FIRMW "+" FLOW_BUILD_REF
#endif

namespace FirmwareVersion {

static constexpr const char* Core = FIRMW;
static constexpr const char* BuildRef = FLOW_BUILD_REF;
static constexpr const char* Full = FLOW_FIRMWARE_VERSION_FULL;

}  // namespace FirmwareVersion


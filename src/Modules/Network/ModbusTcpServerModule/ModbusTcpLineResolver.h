#pragma once
/**
 * @brief Per-port serial-profile resolution without direct UART access.
 *
 * FlowioDevice mode uses an explicit device ID, resolved by the PoolDevice
 * owner. Manual mode uses the configured line. No fallback to a different
 * device or default baud rate is allowed.
 */
#include "ModbusTcpServerPolicy.h"
#include "ModbusTcpResolverRetry.h"

namespace ModbusTcpServer {
class LineResolver {
public:
    using DeviceLookup = bool (*)(void* context, uint16_t deviceId,
                                  SerialLineProfile& out, uint8_t& slaveAddress);
    using DeviceLookupDetailed = ResolveStatus (*)(void* context, uint16_t deviceId,
                                                   SerialLineProfile& out, uint8_t& slaveAddress);
    void configure(uint8_t portIndex, const PortConfig& config) {
        if (!validConfig(portIndex, config)) return;
        configs_[portIndex] = config;
    }
    void setDeviceLookup(DeviceLookup lookup, void* context) {
        lookup_ = lookup;
        detailedLookup_ = nullptr;
        context_ = context;
    }
    void setDeviceLookupDetailed(DeviceLookupDetailed lookup, void* context) {
        detailedLookup_ = lookup;
        lookup_ = nullptr;
        context_ = context;
    }
    ResolveStatus resolveDetailed(uint8_t portIndex, uint8_t unitId, SerialLineProfile& out,
                                  uint8_t& rtuUnitId) const {
        if (!validPortIndex(portIndex) || unitId > 247U) return ResolveStatus::Invalid;
        const PortConfig& config = configs_[portIndex];
        if (!config.enabled || !validConfig(portIndex, config)) return ResolveStatus::Invalid;
        SerialLineProfile line{};
        uint8_t target = unitId;
        if (config.serialSource == SerialSource::Manual) {
            if (!target) return ResolveStatus::Invalid;
            line = config.manualLine;
        } else if (config.serialSource == SerialSource::FlowioDevice) {
            const ResolveStatus result = detailedLookup_
                ? detailedLookup_(context_, config.deviceId, line, target)
                : (lookup_ && lookup_(context_, config.deviceId, line, target)
                    ? ResolveStatus::Ready : ResolveStatus::Invalid);
            if (result != ResolveStatus::Ready) return result;
            if (!target || target > 247U) return ResolveStatus::Invalid;
        } else return ResolveStatus::Invalid;
        if (line.busId != 0U || line.baud < 1200U || line.baud > 115200U ||
            line.parity > 2U || (line.stopBits != 1U && line.stopBits != 2U) ||
            line.minimumRequestIntervalMs > 60000U || line.quietMs > 1000U ||
            line.lateResponseGuardMs > 5000U) return ResolveStatus::Invalid;
        out = line;
        rtuUnitId = target;
        return ResolveStatus::Ready;
    }
    bool resolve(uint8_t portIndex, uint8_t unitId, SerialLineProfile& out,
                 uint8_t& rtuUnitId) const {
        return resolveDetailed(portIndex, unitId, out, rtuUnitId) == ResolveStatus::Ready;
    }
    static ResolveStatus resolveDetailedStatic(void* context, uint8_t portIndex,
                                  uint8_t unitId, SerialLineProfile& out, uint8_t& rtuUnitId) {
        return context ? static_cast<const LineResolver*>(context)->resolveDetailed(
            portIndex, unitId, out, rtuUnitId) : ResolveStatus::Invalid;
    }
    static bool resolveStatic(void* context, uint8_t portIndex, uint8_t unitId,
                              SerialLineProfile& out, uint8_t& rtuUnitId) {
        return context &&
            static_cast<const LineResolver*>(context)->resolve(
                portIndex, unitId, out, rtuUnitId);
    }
private:
    PortConfig configs_[kPortCount]{};
    DeviceLookup lookup_ = nullptr;
    DeviceLookupDetailed detailedLookup_ = nullptr;
    void* context_ = nullptr;
};
} // namespace ModbusTcpServer

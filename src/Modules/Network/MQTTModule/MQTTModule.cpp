/**
 * @file MQTTModule.cpp
 * @brief Facade translation unit for MQTTModule.
 *
 * Architecture: MQTTModule keeps a single public facade and splits its
 * implementation across Lifecycle / Transport / Queue / Rx / Producers
 * translation units.
 */

#include "MQTTModule.h"
#include "Modules/Network/MQTTModule/MQTTRuntime.h"

#include <stdio.h>

bool MQTTModule::writeRuntimeUiValue(uint8_t valueId, IRuntimeUiWriter& writer) const
{
    const RuntimeUiId runtimeId = makeRuntimeUiId(moduleId(), valueId);

    switch (valueId) {
        case RuntimeUiReady:
            return writer.writeBool(runtimeId, dataStore_ ? mqttReady(*dataStore_) : false);

        case RuntimeUiServer: {
            if (cfgData_.host[0] == '\0') return writer.writeUnavailable(runtimeId);
            char server[96] = {0};
            if (cfgData_.port > 0) {
                snprintf(server, sizeof(server), "%s:%ld", cfgData_.host, (long)cfgData_.port);
            } else {
                snprintf(server, sizeof(server), "%s", cfgData_.host);
            }
            return writer.writeString(runtimeId, server);
        }

        case RuntimeUiRxDrop:
            return writer.writeU32(runtimeId, dataStore_ ? mqttRxDrop(*dataStore_) : 0U);
        case RuntimeUiParseFail:
            return writer.writeU32(runtimeId, dataStore_ ? mqttParseFail(*dataStore_) : 0U);
        case RuntimeUiHandlerFail:
            return writer.writeU32(runtimeId, dataStore_ ? mqttHandlerFail(*dataStore_) : 0U);
        case RuntimeUiOversizeDrop:
            return writer.writeU32(runtimeId, dataStore_ ? mqttOversizeDrop(*dataStore_) : 0U);
        default:
            return false;
    }
}

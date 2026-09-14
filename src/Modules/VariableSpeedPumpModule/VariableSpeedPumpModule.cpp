/**
 * @file VariableSpeedPumpModule.cpp
 * @brief Variable-speed pump module implementation.
 */

#include "VariableSpeedPumpModule.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <new>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::VariableSpeedPumpModule)
#include "Core/ModuleLog.h"

bool VariableSpeedPumpModule::ensureStorage_()
{
    if (!storageAllocationAttempted_) {
        storageAllocationAttempted_ = true;
        void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) {
            storage_ = new (memory) Storage{};
            LOGI("Pump storage ready bytes=%u memory=psram", (unsigned)sizeof(Storage));
        } else {
            LOGE("Pump service unavailable: PSRAM allocation failed bytes=%u", (unsigned)sizeof(Storage));
        }
    }
    return storage_ != nullptr;
}

bool VariableSpeedPumpModule::definePump(const VariableSpeedPumpDefinition& definition)
{
    if (definition.id >= VARIABLE_SPEED_PUMP_MAX ||
        definition.modbus.ownerId == 0U ||
        definition.modbus.slaveAddress == 0U ||
        definition.modbus.slaveAddress > 247U) {
        return false;
    }
    if (!ensureStorage_()) return false;
    Slot& slot = storage_->slots[definition.id];
    if (slot.used) return false;
    slot.used = true;
    slot.definition = definition;
    slot.driver.configure(definition.modbus);
    return true;
}

void VariableSpeedPumpModule::init(ConfigStore&, ServiceRegistry& services)
{
    ensureStorage_();
    mutex_ = xSemaphoreCreateMutexStatic(&mutexStorage_);
    modbus_ = services.get<ModbusMasterService>(ServiceId::ModbusMaster);
    if (!mutex_) LOGE("State mutex creation failed");
    if (!modbus_) LOGE("Missing service: %s", toString(ServiceId::ModbusMaster));
    if (!services.add(ServiceId::VariableSpeedPump, &service_)) {
        LOGE("Service registration failed: %s", toString(ServiceId::VariableSpeedPump));
    }
}

void VariableSpeedPumpModule::onStart(ConfigStore&, ServiceRegistry&)
{
    if (!lock_()) return;
    for (Slot& slot : storage_->slots) {
        if (!slot.used || !slot.definition.modbus.enabled) continue;
        slot.started = slot.driver.begin(modbus_);
        if (!slot.started) {
            LOGE("Pump %u driver start failed slave=%u",
                 (unsigned)slot.definition.id,
                 (unsigned)slot.definition.modbus.slaveAddress);
        } else {
            LOGI("Pump %u ready slave=%u poll_ms=%lu",
                 (unsigned)slot.definition.id,
                 (unsigned)slot.definition.modbus.slaveAddress,
                 (unsigned long)slot.definition.modbus.pollIntervalMs);
        }
    }
    unlock_();
}

void VariableSpeedPumpModule::loop()
{
    if (lock_(pdMS_TO_TICKS(10U))) {
        const uint32_t nowMs = millis();
        for (Slot& slot : storage_->slots) {
            if (slot.used && slot.started) slot.driver.tick(nowMs);
        }
        unlock_();
    }
    vTaskDelay(pdMS_TO_TICKS(20U));
}

bool VariableSpeedPumpModule::lock_(TickType_t timeout) const
{
    return storage_ && mutex_ && xSemaphoreTake(mutex_, timeout) == pdTRUE;
}

void VariableSpeedPumpModule::unlock_() const
{
    if (mutex_) xSemaphoreGive(mutex_);
}

uint8_t VariableSpeedPumpModule::countStatic_(void* ctx)
{
    return ctx ? static_cast<VariableSpeedPumpModule*>(ctx)->count_() : 0U;
}

VariableSpeedPumpStatus VariableSpeedPumpModule::readStateStatic_(
    void* ctx, VariableSpeedPumpId pumpId, VariableSpeedPumpState* outState)
{
    if (!ctx || !outState) return VARIABLE_SPEED_PUMP_INVALID_ARGUMENT;
    return static_cast<VariableSpeedPumpModule*>(ctx)->readState_(pumpId, *outState);
}

VariableSpeedPumpStatus VariableSpeedPumpModule::setRunningStatic_(
    void* ctx, VariableSpeedPumpId pumpId, bool running)
{
    if (!ctx) return VARIABLE_SPEED_PUMP_INVALID_ARGUMENT;
    return static_cast<VariableSpeedPumpModule*>(ctx)->setRunning_(pumpId, running);
}

VariableSpeedPumpStatus VariableSpeedPumpModule::setSpeedStatic_(
    void* ctx, VariableSpeedPumpId pumpId, float speedPercent)
{
    if (!ctx) return VARIABLE_SPEED_PUMP_INVALID_ARGUMENT;
    return static_cast<VariableSpeedPumpModule*>(ctx)->setSpeed_(pumpId, speedPercent);
}

uint8_t VariableSpeedPumpModule::count_() const
{
    if (!lock_()) return 0U;
    uint8_t count = 0U;
    for (const Slot& slot : storage_->slots) {
        if (slot.used) ++count;
    }
    unlock_();
    return count;
}

VariableSpeedPumpStatus VariableSpeedPumpModule::readState_(
    VariableSpeedPumpId pumpId, VariableSpeedPumpState& outState) const
{
    if (pumpId >= VARIABLE_SPEED_PUMP_MAX) return VARIABLE_SPEED_PUMP_UNKNOWN;
    if (!lock_()) return VARIABLE_SPEED_PUMP_NOT_READY;
    const Slot& slot = storage_->slots[pumpId];
    VariableSpeedPumpStatus result = VARIABLE_SPEED_PUMP_UNKNOWN;
    if (slot.used && !slot.definition.modbus.enabled) {
        result = VARIABLE_SPEED_PUMP_DISABLED;
    } else if (slot.used && slot.started && slot.driver.readState(outState)) {
        result = !outState.valid ? VARIABLE_SPEED_PUMP_NOT_READY
                 : outState.online ? VARIABLE_SPEED_PUMP_OK
                                   : VARIABLE_SPEED_PUMP_COMMUNICATION_ERROR;
    } else if (slot.used) {
        result = VARIABLE_SPEED_PUMP_NOT_READY;
    }
    unlock_();
    return result;
}

VariableSpeedPumpStatus VariableSpeedPumpModule::setRunning_(
    VariableSpeedPumpId pumpId, bool running)
{
    if (pumpId >= VARIABLE_SPEED_PUMP_MAX) return VARIABLE_SPEED_PUMP_UNKNOWN;
    if (!lock_()) return VARIABLE_SPEED_PUMP_NOT_READY;
    Slot& slot = storage_->slots[pumpId];
    const VariableSpeedPumpStatus result = !slot.used
                                               ? VARIABLE_SPEED_PUMP_UNKNOWN
                                               : slot.driver.setRunning(running);
    unlock_();
    return result;
}

VariableSpeedPumpStatus VariableSpeedPumpModule::setSpeed_(
    VariableSpeedPumpId pumpId, float speedPercent)
{
    if (pumpId >= VARIABLE_SPEED_PUMP_MAX) return VARIABLE_SPEED_PUMP_UNKNOWN;
    if (!lock_()) return VARIABLE_SPEED_PUMP_NOT_READY;
    Slot& slot = storage_->slots[pumpId];
    const VariableSpeedPumpStatus result = !slot.used
                                               ? VARIABLE_SPEED_PUMP_UNKNOWN
                                               : slot.driver.setSpeedPercent(speedPercent);
    unlock_();
    return result;
}

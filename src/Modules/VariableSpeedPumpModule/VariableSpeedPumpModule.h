#pragma once
/**
 * @file VariableSpeedPumpModule.h
 * @brief Domain module exposing descriptor-driven variable-speed pumps.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Core/Module.h"
#include "Core/Services/IModbusMaster.h"
#include "Core/Services/IVariableSpeedPump.h"
#include "Modules/VariableSpeedPumpModule/Drivers/ModbusVariableSpeedPumpDriver.h"

constexpr uint8_t VARIABLE_SPEED_PUMP_MAX = 4U;

struct VariableSpeedPumpDefinition {
    VariableSpeedPumpId id = VARIABLE_SPEED_PUMP_INVALID;
    ModbusVariableSpeedPumpConfig modbus{};
};

class VariableSpeedPumpModule : public Module {
public:
    ModuleId moduleId() const override { return ModuleId::VariableSpeedPump; }
    const char* taskName() const override { return "pumpvar"; }
    uint8_t dependencyCount() const override { return 2U; }
    ModuleId dependency(uint8_t index) const override {
        if (index == 0U) return ModuleId::LogHub;
        if (index == 1U) return ModuleId::Io;
        return ModuleId::Unknown;
    }
    uint8_t taskCount() const override { return 1U; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }
    uint16_t taskStackSize() const override { return 3072U; }
    BaseType_t taskCore() const override { return 1; }
    uint32_t startDelayMs() const override { return 1000U; }

    // Boot-time profile assembly only, before module tasks are started.
    bool definePump(const VariableSpeedPumpDefinition& definition);
    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onStart(ConfigStore& cfg, ServiceRegistry& services) override;
    void loop() override;

private:
    struct Slot {
        bool used = false;
        bool started = false;
        VariableSpeedPumpDefinition definition{};
        ModbusVariableSpeedPumpDriver driver{};
    };

    struct Storage {
        Slot slots[VARIABLE_SPEED_PUMP_MAX]{};
    };

    bool ensureStorage_();

    static uint8_t countStatic_(void* ctx);
    static VariableSpeedPumpStatus readStateStatic_(void* ctx,
                                                     VariableSpeedPumpId pumpId,
                                                     VariableSpeedPumpState* outState);
    static VariableSpeedPumpStatus setRunningStatic_(void* ctx,
                                                      VariableSpeedPumpId pumpId,
                                                      bool running);
    static VariableSpeedPumpStatus setSpeedStatic_(void* ctx,
                                                    VariableSpeedPumpId pumpId,
                                                    float speedPercent);

    uint8_t count_() const;
    VariableSpeedPumpStatus readState_(VariableSpeedPumpId pumpId,
                                        VariableSpeedPumpState& outState) const;
    VariableSpeedPumpStatus setRunning_(VariableSpeedPumpId pumpId, bool running);
    VariableSpeedPumpStatus setSpeed_(VariableSpeedPumpId pumpId, float speedPercent);
    bool lock_(TickType_t timeout = pdMS_TO_TICKS(50U)) const;
    void unlock_() const;

    // Allocated once in PSRAM and retained for the firmware lifetime.
    Storage* storage_ = nullptr;
    bool storageAllocationAttempted_ = false;
    const ModbusMasterService* modbus_ = nullptr;
    mutable StaticSemaphore_t mutexStorage_{};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    VariableSpeedPumpService service_{
        &VariableSpeedPumpModule::countStatic_,
        &VariableSpeedPumpModule::readStateStatic_,
        &VariableSpeedPumpModule::setRunningStatic_,
        &VariableSpeedPumpModule::setSpeedStatic_,
        this
    };
};

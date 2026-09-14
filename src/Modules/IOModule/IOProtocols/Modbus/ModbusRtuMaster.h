#pragma once
/**
 * @file ModbusRtuMaster.h
 * @brief Fixed-capacity asynchronous Modbus RTU transaction engine.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "Core/Services/IModbusMaster.h"
#include "Modules/IOModule/IOBus/Rs485Bus.h"

class ModbusRtuMaster {
public:
    explicit ModbusRtuMaster(Rs485Bus& bus);

    bool begin();
    void tick(uint32_t nowMs, uint32_t nowUs);
    ModbusMasterService& service() { return service_; }

private:
    static constexpr uint8_t kQueueCapacity = 8U;
    enum class SlotState : uint8_t { Free, Queued, Active, Finished };

    struct Slot {
        SlotState state = SlotState::Free;
        uint16_t transactionId = MODBUS_TRANSACTION_INVALID;
        uint32_t order = 0U;
        uint32_t sentAtMs = 0U;
        uint8_t attempts = 0U;
        ModbusRequest request{};
        ModbusResponse response{};
    };

    struct Storage {
        Slot slots[kQueueCapacity]{};
        uint8_t txFrame[RS485_MAX_FRAME_BYTES]{};
        uint8_t rxFrame[RS485_MAX_FRAME_BYTES]{};
    };

    static ModbusResultCode submitStatic_(void* ctx,
                                           const ModbusRequest* request,
                                           uint16_t* outTransactionId);
    static ModbusResultCode pollStatic_(void* ctx,
                                         uint16_t transactionId,
                                         ModbusResponse* outResponse);
    static void cancelOwnerStatic_(void* ctx, uint8_t ownerId);
    static bool statsStatic_(void* ctx, ModbusMasterStats* outStats);

    ModbusResultCode submit_(const ModbusRequest& request, uint16_t& outTransactionId);
    ModbusResultCode poll_(uint16_t transactionId, ModbusResponse& outResponse);
    void cancelOwner_(uint8_t ownerId);
    bool stats_(ModbusMasterStats& outStats) const;
    bool lock_(TickType_t timeout = pdMS_TO_TICKS(20U)) const;
    void unlock_() const;
    uint16_t allocateTransactionId_();
    int8_t selectNextSlot_() const;
    bool sendActive_(Slot& slot, uint32_t nowMs, uint32_t nowUs);
    void finish_(Slot& slot, ModbusResultCode result, const ModbusResponse* response = nullptr);
    void retryOrFinish_(Slot& slot, ModbusResultCode result, uint32_t nowMs, uint32_t nowUs);
    static bool validRequest_(const ModbusRequest& request);

    Rs485Bus& bus_;
    mutable StaticSemaphore_t mutexStorage_{};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    // Allocated once in PSRAM and retained for the firmware lifetime.
    Storage* storage_ = nullptr;
    bool storageAllocationAttempted_ = false;
    int8_t activeIndex_ = -1;
    uint16_t nextTransactionId_ = 1U;
    uint32_t nextOrder_ = 1U;
    bool ready_ = false;
    ModbusMasterStats statsData_{};
    ModbusMasterService service_{};
};

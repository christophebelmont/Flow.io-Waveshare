/**
 * @file ModbusRtuMaster.cpp
 * @brief Asynchronous Modbus RTU master implementation.
 */

#include "ModbusRtuMaster.h"

#include <esp_heap_caps.h>
#include <new>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

#include "Modules/IOModule/IOProtocols/Modbus/ModbusRtuCodec.h"

ModbusRtuMaster::ModbusRtuMaster(Rs485Bus& bus) : bus_(bus)
{
    service_ = {
        &ModbusRtuMaster::submitStatic_,
        &ModbusRtuMaster::pollStatic_,
        &ModbusRtuMaster::cancelOwnerStatic_,
        &ModbusRtuMaster::statsStatic_,
        this
    };
}

bool ModbusRtuMaster::begin()
{
    if (!mutex_) mutex_ = xSemaphoreCreateMutexStatic(&mutexStorage_);
    if (!storageAllocationAttempted_ && mutex_ && bus_.ready()) {
        storageAllocationAttempted_ = true;
        void* memory = heap_caps_malloc(sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory) {
            storage_ = new (memory) Storage{};
            LOGI("Modbus storage ready bytes=%u memory=psram", (unsigned)sizeof(Storage));
        } else {
            LOGE("Modbus unavailable: PSRAM allocation failed bytes=%u", (unsigned)sizeof(Storage));
        }
    }
    ready_ = mutex_ != nullptr && storage_ != nullptr && bus_.ready();
    statsData_.ready = ready_;
    return ready_;
}

bool ModbusRtuMaster::lock_(TickType_t timeout) const
{
    return mutex_ && xSemaphoreTake(mutex_, timeout) == pdTRUE;
}

void ModbusRtuMaster::unlock_() const
{
    if (mutex_) xSemaphoreGive(mutex_);
}

bool ModbusRtuMaster::validRequest_(const ModbusRequest& request)
{
    if (request.ownerId == 0U || request.slaveAddress == 0U ||
        request.slaveAddress > 247U || request.registerCount == 0U ||
        request.registerCount > MODBUS_MAX_REGISTERS_PER_REQUEST ||
        request.responseTimeoutMs == 0U || request.priority > MODBUS_PRIORITY_SAFETY) {
        return false;
    }
    switch (request.function) {
        case MODBUS_FC_READ_HOLDING_REGISTERS:
        case MODBUS_FC_READ_INPUT_REGISTERS:
        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            return true;
        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            return request.registerCount == 1U;
        default:
            return false;
    }
}

ModbusResultCode ModbusRtuMaster::submitStatic_(void* ctx,
                                                const ModbusRequest* request,
                                                uint16_t* outTransactionId)
{
    if (!ctx || !request || !outTransactionId) return MODBUS_RESULT_INVALID_ARGUMENT;
    return static_cast<ModbusRtuMaster*>(ctx)->submit_(*request, *outTransactionId);
}

ModbusResultCode ModbusRtuMaster::pollStatic_(void* ctx,
                                              uint16_t transactionId,
                                              ModbusResponse* outResponse)
{
    if (!ctx || !outResponse || transactionId == MODBUS_TRANSACTION_INVALID) {
        return MODBUS_RESULT_INVALID_ARGUMENT;
    }
    return static_cast<ModbusRtuMaster*>(ctx)->poll_(transactionId, *outResponse);
}

void ModbusRtuMaster::cancelOwnerStatic_(void* ctx, uint8_t ownerId)
{
    if (ctx) static_cast<ModbusRtuMaster*>(ctx)->cancelOwner_(ownerId);
}

bool ModbusRtuMaster::statsStatic_(void* ctx, ModbusMasterStats* outStats)
{
    return ctx && outStats && static_cast<ModbusRtuMaster*>(ctx)->stats_(*outStats);
}

ModbusResultCode ModbusRtuMaster::submit_(const ModbusRequest& request,
                                          uint16_t& outTransactionId)
{
    outTransactionId = MODBUS_TRANSACTION_INVALID;
    if (!validRequest_(request)) return MODBUS_RESULT_INVALID_ARGUMENT;
    if (!ready_) return MODBUS_RESULT_NOT_READY;
    if (!lock_()) return MODBUS_RESULT_NOT_READY;

    Slot* freeSlot = nullptr;
    for (Slot& slot : storage_->slots) {
        if (slot.state == SlotState::Free) {
            freeSlot = &slot;
            break;
        }
    }
    if (!freeSlot) {
        unlock_();
        return MODBUS_RESULT_QUEUE_FULL;
    }

    const uint16_t id = allocateTransactionId_();
    if (id == MODBUS_TRANSACTION_INVALID) {
        unlock_();
        return MODBUS_RESULT_QUEUE_FULL;
    }
    *freeSlot = Slot{};
    freeSlot->state = SlotState::Queued;
    freeSlot->transactionId = id;
    freeSlot->order = nextOrder_++;
    freeSlot->request = request;
    freeSlot->response.transactionId = id;
    outTransactionId = id;
    ++statsData_.submitted;
    unlock_();
    return MODBUS_RESULT_OK;
}

uint16_t ModbusRtuMaster::allocateTransactionId_()
{
    // The queue is small, but completed transactions may remain until consumed.
    // Avoid reusing their identifier when the 16-bit sequence wraps.
    for (uint32_t checked = 0U; checked < UINT16_MAX; ++checked) {
        uint16_t candidate = nextTransactionId_++;
        if (candidate == MODBUS_TRANSACTION_INVALID) candidate = nextTransactionId_++;
        bool inUse = false;
        for (const Slot& slot : storage_->slots) {
            if (slot.state != SlotState::Free && slot.transactionId == candidate) {
                inUse = true;
                break;
            }
        }
        if (!inUse) return candidate;
    }
    return MODBUS_TRANSACTION_INVALID;
}

ModbusResultCode ModbusRtuMaster::poll_(uint16_t transactionId,
                                        ModbusResponse& outResponse)
{
    if (!storage_) return MODBUS_RESULT_NOT_READY;
    if (!lock_()) return MODBUS_RESULT_NOT_READY;
    for (Slot& slot : storage_->slots) {
        if (slot.state == SlotState::Free || slot.transactionId != transactionId) continue;
        if (slot.state != SlotState::Finished) {
            outResponse = slot.response;
            outResponse.transactionId = transactionId;
            outResponse.state = MODBUS_TRANSACTION_QUEUED;
            outResponse.result = MODBUS_RESULT_NOT_READY;
            unlock_();
            return MODBUS_RESULT_NOT_READY;
        }
        outResponse = slot.response;
        slot = Slot{};
        unlock_();
        return MODBUS_RESULT_OK;
    }
    unlock_();
    return MODBUS_RESULT_UNKNOWN_TRANSACTION;
}

void ModbusRtuMaster::cancelOwner_(uint8_t ownerId)
{
    if (ownerId == 0U || !storage_ || !lock_()) return;
    for (uint8_t i = 0U; i < kQueueCapacity; ++i) {
        Slot& slot = storage_->slots[i];
        if (slot.state == SlotState::Free || slot.request.ownerId != ownerId) continue;
        if ((int8_t)i == activeIndex_) {
            bus_.abort();
            activeIndex_ = -1;
        }
        slot = Slot{};
    }
    unlock_();
}

bool ModbusRtuMaster::stats_(ModbusMasterStats& outStats) const
{
    if (!lock_()) return false;
    outStats = statsData_;
    outStats.queued = 0U;
    if (!storage_) {
        unlock_();
        return true;
    }
    for (const Slot& slot : storage_->slots) {
        if (slot.state == SlotState::Queued || slot.state == SlotState::Active) {
            ++outStats.queued;
        }
    }
    unlock_();
    return true;
}

int8_t ModbusRtuMaster::selectNextSlot_() const
{
    int8_t selected = -1;
    for (uint8_t i = 0U; i < kQueueCapacity; ++i) {
        const Slot& candidate = storage_->slots[i];
        if (candidate.state != SlotState::Queued) continue;
        if (selected < 0) {
            selected = (int8_t)i;
            continue;
        }
        const Slot& current = storage_->slots[(uint8_t)selected];
        if (candidate.request.priority > current.request.priority ||
            (candidate.request.priority == current.request.priority &&
             candidate.order < current.order)) {
            selected = (int8_t)i;
        }
    }
    return selected;
}

bool ModbusRtuMaster::sendActive_(Slot& slot, uint32_t nowMs, uint32_t nowUs)
{
    size_t txLength = 0U;
    if (!ModbusRtuCodec::encodeRequest(slot.request,
                                       storage_->txFrame, sizeof(storage_->txFrame), txLength)) {
        finish_(slot, MODBUS_RESULT_INVALID_ARGUMENT);
        return false;
    }
    if (!bus_.startTransmit(storage_->txFrame, txLength, nowUs)) {
        ++statsData_.ioErrors;
        finish_(slot, MODBUS_RESULT_IO_ERROR);
        return false;
    }
    slot.state = SlotState::Active;
    slot.sentAtMs = nowMs;
    return true;
}

void ModbusRtuMaster::finish_(Slot& slot,
                              ModbusResultCode result,
                              const ModbusResponse* response)
{
    if (response) slot.response = *response;
    slot.response.transactionId = slot.transactionId;
    slot.response.result = result;
    slot.response.state = (result == MODBUS_RESULT_OK)
                              ? MODBUS_TRANSACTION_COMPLETE
                              : MODBUS_TRANSACTION_FAILED;
    slot.state = SlotState::Finished;
    activeIndex_ = -1;
    if (result == MODBUS_RESULT_OK) ++statsData_.completed;
}

void ModbusRtuMaster::retryOrFinish_(Slot& slot,
                                     ModbusResultCode result,
                                     uint32_t nowMs,
                                     uint32_t nowUs)
{
    if (result == MODBUS_RESULT_CRC_ERROR) ++statsData_.crcErrors;
    if (result == MODBUS_RESULT_PROTOCOL_ERROR) ++statsData_.protocolErrors;
    if (result == MODBUS_RESULT_TIMEOUT) ++statsData_.timeouts;
    bus_.abort();
    if (result != MODBUS_RESULT_EXCEPTION && slot.attempts < slot.request.retries) {
        ++slot.attempts;
        ++statsData_.retries;
        (void)sendActive_(slot, nowMs, nowUs);
        return;
    }
    finish_(slot, result);
}

void ModbusRtuMaster::tick(uint32_t nowMs, uint32_t nowUs)
{
    if (!ready_ || !lock_(0U)) return;
    bus_.tick(nowUs);

    if (activeIndex_ < 0) {
        activeIndex_ = selectNextSlot_();
        if (activeIndex_ >= 0) {
            Slot& slot = storage_->slots[(uint8_t)activeIndex_];
            (void)sendActive_(slot, nowMs, nowUs);
        }
        unlock_();
        return;
    }

    Slot& slot = storage_->slots[(uint8_t)activeIndex_];
    if (bus_.frameAvailable()) {
        size_t rxLength = 0U;
        if (!bus_.takeFrame(storage_->rxFrame, sizeof(storage_->rxFrame), rxLength)) {
            retryOrFinish_(slot, MODBUS_RESULT_IO_ERROR, nowMs, nowUs);
        } else {
            ModbusResponse response{};
            const ModbusResultCode result = ModbusRtuCodec::decodeResponse(
                slot.request, storage_->rxFrame, rxLength, response);
            if (result == MODBUS_RESULT_OK || result == MODBUS_RESULT_EXCEPTION) {
                finish_(slot, result, &response);
            } else {
                retryOrFinish_(slot, result, nowMs, nowUs);
            }
        }
    } else if ((uint32_t)(nowMs - slot.sentAtMs) >= slot.request.responseTimeoutMs) {
        retryOrFinish_(slot, MODBUS_RESULT_TIMEOUT, nowMs, nowUs);
    }
    unlock_();
}

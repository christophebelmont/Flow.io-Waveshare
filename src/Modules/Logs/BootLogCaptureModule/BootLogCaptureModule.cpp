/**
 * @file BootLogCaptureModule.cpp
 * @brief Passive boot log capture sink backed by PSRAM.
 */

#include "BootLogCaptureModule.h"

#if FLOW_ENABLE_BOOT_LOG_CAPTURE

#include "Core/LogModuleIds.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::BootLogCaptureModule)
#include "Core/ModuleLog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {
const BootLogCaptureService* gBootLogCaptureService = nullptr;
}

const BootLogCaptureService* bootLogCaptureService()
{
    return gBootLogCaptureService;
}

void markBootLogCaptureComplete()
{
    if (gBootLogCaptureService && gBootLogCaptureService->markComplete) {
        gBootLogCaptureService->markComplete(gBootLogCaptureService->ctx);
    }
}

void BootLogCaptureModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    (void)cfg;

    service_.markComplete = &BootLogCaptureModule::serviceMarkComplete_;
    service_.getStats = &BootLogCaptureModule::serviceGetStats_;
    service_.replay = &BootLogCaptureModule::serviceReplay_;
    service_.readPage = &BootLogCaptureModule::serviceReadPage_;
    service_.ctx = this;
    gBootLogCaptureService = &service_;

    if (!psramFound()) {
        LOGW("Boot log capture disabled: PSRAM not found");
        return;
    }

    const size_t bytes = (size_t)kCapacity * sizeof(LogEntry);
    entries_ = static_cast<LogEntry*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries_) {
        LOGW("Boot log capture disabled: PSRAM alloc failed size=%u", (unsigned)bytes);
        return;
    }

    capacity_ = kCapacity;
    inPsram_ = true;
    capturing_ = true;

    const LogSinkRegistryService* sinks = services.get<LogSinkRegistryService>(ServiceId::LogSinks);
    if (!sinks || !sinks->add) {
        capturing_ = false;
        LOGW("Boot log capture disabled: log sink registry missing");
        return;
    }

    const LogSinkService sink{&BootLogCaptureModule::sinkWrite_, this};
    if (!sinks->add(sinks->ctx, sink)) {
        capturing_ = false;
        LOGW("Boot log capture disabled: log sink registry full");
        return;
    }

    LOGI("Boot log capture enabled entries=%u bytes=%u psram_free=%luKB",
         (unsigned)capacity_,
         (unsigned)bytes,
         (unsigned long)(ESP.getFreePsram() / 1024U));
}

void BootLogCaptureModule::sinkWrite_(void* ctx, const LogEntry& e)
{
    BootLogCaptureModule* self = static_cast<BootLogCaptureModule*>(ctx);
    if (!self) return;
    self->write_(e);
}

void BootLogCaptureModule::serviceMarkComplete_(void* ctx)
{
    BootLogCaptureModule* self = static_cast<BootLogCaptureModule*>(ctx);
    if (!self) return;
    self->markComplete_();
}

void BootLogCaptureModule::serviceGetStats_(void* ctx, BootLogCaptureStats* out)
{
    BootLogCaptureModule* self = static_cast<BootLogCaptureModule*>(ctx);
    if (!self || !out) return;
    self->getStats_(*out);
}

uint16_t BootLogCaptureModule::serviceReplay_(void* ctx,
                                              BootLogCaptureReplayWriter writer,
                                              void* writerCtx)
{
    BootLogCaptureModule* self = static_cast<BootLogCaptureModule*>(ctx);
    if (!self) return 0;
    return self->replay_(writer, writerCtx);
}

uint16_t BootLogCaptureModule::serviceReadPage_(void* ctx,
                                                uint16_t offset,
                                                uint16_t limit,
                                                BootLogCaptureReplayWriter writer,
                                                void* writerCtx)
{
    BootLogCaptureModule* self = static_cast<BootLogCaptureModule*>(ctx);
    if (!self) return 0;
    return self->readPage_(offset, limit, writer, writerCtx);
}

void BootLogCaptureModule::write_(const LogEntry& e)
{
    if (!entries_ || capacity_ == 0) return;

    portENTER_CRITICAL(&mux_);
    if (!capturing_) {
        portEXIT_CRITICAL(&mux_);
        return;
    }

    uint16_t idx = 0;
    if (count_ < capacity_) {
        idx = (uint16_t)((head_ + count_) % capacity_);
        ++count_;
    } else {
        idx = head_;
        head_ = (uint16_t)((head_ + 1U) % capacity_);
        ++droppedCount_;
    }
    entries_[idx] = e;
    portEXIT_CRITICAL(&mux_);
}

void BootLogCaptureModule::markComplete_()
{
    bool changed = false;
    uint16_t captured = 0;
    uint32_t dropped = 0;
    portENTER_CRITICAL(&mux_);
    if (capturing_) {
        capturing_ = false;
        complete_ = true;
        changed = true;
        captured = count_;
        dropped = droppedCount_;
    } else if (!complete_) {
        complete_ = true;
    }
    portEXIT_CRITICAL(&mux_);

    if (changed) {
        LOGI("Boot log capture complete entries=%u dropped=%lu",
             (unsigned)captured,
             (unsigned long)dropped);
    }
}

void BootLogCaptureModule::getStats_(BootLogCaptureStats& out) const
{
    portENTER_CRITICAL(&mux_);
    out.capacity = capacity_;
    out.count = count_;
    out.droppedCount = droppedCount_;
    out.capturing = capturing_;
    out.complete = complete_;
    out.psram = inPsram_;
    portEXIT_CRITICAL(&mux_);
}

uint16_t BootLogCaptureModule::replay_(BootLogCaptureReplayWriter writer, void* writerCtx) const
{
    return readPage_(0, UINT16_MAX, writer, writerCtx);
}

uint16_t BootLogCaptureModule::readPage_(uint16_t offset,
                                         uint16_t limit,
                                         BootLogCaptureReplayWriter writer,
                                         void* writerCtx) const
{
    if (!writer || !entries_ || capacity_ == 0) return 0;

    uint16_t total = 0;
    portENTER_CRITICAL(&mux_);
    total = count_;
    portEXIT_CRITICAL(&mux_);
    if (offset >= total || limit == 0U) return 0;

    const uint16_t available = (uint16_t)(total - offset);
    const uint16_t maxToSend = (limit < available) ? limit : available;
    uint16_t sent = 0;
    for (uint16_t i = 0; i < maxToSend; ++i) {
        LogEntry entry{};
        portENTER_CRITICAL(&mux_);
        const uint16_t logicalIndex = (uint16_t)(offset + i);
        const uint16_t idx = (uint16_t)((head_ + logicalIndex) % capacity_);
        entry = entries_[idx];
        portEXIT_CRITICAL(&mux_);

        if (!writer(writerCtx, entry, logicalIndex, total)) {
            break;
        }
        ++sent;
    }
    return sent;
}

#endif

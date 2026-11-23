#pragma once
#include <cassert>
#include <cstring>
#include <functional>
#include "Core/Services/IIO.h"
#include "Core/Services/IConfig.h"
#include "Core/CommandRegistry.h"
#include "Core/Values/PulseRuntime.h"
#include "Core/Values/PulseCheckpoint.h"

inline uint32_t testNow = 0;
inline std::function<void()> testTick;
inline uint32_t millis() { return testNow; }
inline void vTaskDelay(uint32_t ms) { testNow += ms; if (testTick) testTick(); }
#define pdMS_TO_TICKS(ms) (ms)
namespace Limits { namespace Config { namespace Capacity { constexpr size_t RuntimeBlobAsyncMax = 512; } } }
constexpr uint8_t MAX_DIGITAL_INPUTS = 2, MAX_DIGITAL_SLOTS = 2;
constexpr uint8_t DIGITAL_SLOT_INPUT = 1, IO_DIGITAL_INPUT_COUNTER = 1;
struct IDigitalCounterDriver {
    uint64_t raw = 0;
    bool readable = true;
    bool readCount(uint64_t& value) { value = raw; return readable; }
};
struct FakeStore {
    uint8_t bytes[PulseCheckpoint::Size]{};
    size_t length = 0;
    bool readRuntimeBlob(const char*, void* out, size_t size, size_t* len) {
        *len = length; if (length) memcpy(out, bytes, size); return length != 0;
    }
    bool writeRuntimeBlob(const char*, const void* value, size_t size) {
        memcpy(bytes, value, size); length = size; return true;
    }
};
// Hardware and task boundary fixture. All persistence/reset methods are compiled
// from IOPulsePersistence.cpp unchanged; only its platform-heavy module header is replaced.
struct IOModule {
    enum class PulseRequestState : uint8_t { Idle, Requested, Saving, Complete };
    std::atomic<PulseRequestState> pulseRequestState_{PulseRequestState::Idle};
    std::atomic_flag pulseRequestCaller_ = ATOMIC_FLAG_INIT;
    static constexpr uint32_t PulseRequestTimeoutMs = 2000;
    uint32_t pulseRequestStartedMs_ = 0;
    IoId pulseRequestResetId_ = IO_ID_INVALID;
    bool pulseRequestSetValue_ = false;
    uint64_t pulseRequestCount_ = 0;
    IoStatus pulseRequestResult_ = IO_OK;
    bool pulseRequestSubmitted_ = false;
    struct Slot {
        bool used = true;
        uint8_t kind = DIGITAL_SLOT_INPUT, logicalIdx = 0;
        struct { uint8_t mode = IO_DIGITAL_INPUT_COUNTER; } inDef;
        struct { void* ctx = nullptr; bool isBound() const { return ctx; } } provider;
        void* endpoint = this;
        PulseRuntime pulse;
        uint16_t counterResetSeen = 0;
    } slots[2];
    Slot* digitalSlots_ = slots;
    IDigitalCounterDriver drivers[2];
    struct { uint16_t counterReset = 0; } digitalInCfg_[2];
    struct { bool enabled = true; } cfgData_;
    PulseCheckpoint::State pulsePersisted_{}, pulsePending_{};
    PersistenceReceipt pulseReceipt_{};
    uint32_t pulseLastAttemptMs_ = 0, pulseCheckpointWrites_ = 0, pulseCheckpointFailures_ = 0;
    bool pulseStorageReady_ = true, pulseRetry_ = false, runtimeReady_ = true;
    FakeStore store;
    FakeStore* cfgStore_ = &store;
    ConfigStoreService service{};
    ConfigStoreService* cfgSvc_ = &service;
    bool enqueueOk = true;
    unsigned writes = 0;
    PulseCheckpoint::State queued{};
    IOModule() {
        service.ctx = this;
        service.writeRuntimeBlobTracked = [](void* ctx, const char*, const void* bytes, size_t len, PersistenceReceipt* receipt) {
            auto& self = *static_cast<IOModule*>(ctx);
            if (!self.enqueueOk) return false;
            assert(receipt->status.load() == PersistenceReceipt::Idle);
            assert(PulseCheckpoint::decode(static_cast<const uint8_t*>(bytes), len, self.queued));
            receipt->status.store(PersistenceReceipt::Pending);
            ++self.writes;
            return true;
        };
        for (unsigned i = 0; i < 2; ++i) {
            slots[i].logicalIdx = i; slots[i].provider.ctx = &drivers[i];
            slots[i].pulse.restore(0, 0, testNow);
        }
    }
    bool findDigitalSlotByIoId_(IoId id, uint8_t& slot) {
        if (id < IO_ID_DI_BASE || id >= IO_ID_DI_BASE + 2) return false;
        slot = id - IO_ID_DI_BASE; return slots[slot].used;
    }
    bool processDigitalInputDefinition_(uint8_t i, uint32_t now) {
        uint64_t raw;
        return drivers[i].readCount(raw) && slots[i].pulse.sample(raw, now);
    }
    bool loadPulseCheckpoint_();
    void checkpointPulses_(uint32_t, bool force = false);
    bool servicePulseRequest_(uint32_t);
    IoStatus requestPulseSave_(IoId, bool setValue = false, uint64_t desiredCount = 0);
    IoStatus resetCounter_(IoId);
    IoStatus setCounter_(IoId, uint64_t);
    IoStatus saveCounters_();
    static bool cmdResetCounter_(void*, const CommandRequest&, char*, size_t);
    static bool cmdSetCounter_(void*, const CommandRequest&, char*, size_t);
    void request(IoId id = IO_ID_INVALID, bool setValue = false, uint64_t desiredCount = 0) {
        pulseRequestResetId_ = id;
        pulseRequestSetValue_ = setValue;
        pulseRequestCount_ = desiredCount;
        pulseRequestStartedMs_ = testNow;
        pulseRequestState_.store(PulseRequestState::Requested);
    }
    void step() { servicePulseRequest_(testNow); }
    void complete(bool ok = true) {
        assert(pulseReceipt_.status.load() == PersistenceReceipt::Pending);
        if (ok) { PulseCheckpoint::encode(queued, store.bytes); store.length = sizeof(store.bytes); }
        pulseReceipt_.status.store(ok ? PersistenceReceipt::Succeeded : PersistenceReceipt::Failed);
    }
};

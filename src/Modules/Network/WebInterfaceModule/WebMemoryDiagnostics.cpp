#include "WebMemoryDiagnostics.h"

#if FLOW_MEMORY_DIAGNOSTICS
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <atomic>
#include <new>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::WebInterfaceModule)
#include "Core/ModuleLog.h"

namespace WebMemoryDiagnostics {
namespace {
constexpr unsigned kCapacity = 24;
struct Record {
    std::atomic<unsigned> owners{0};
    uint32_t id = 0;
};
Record* records = nullptr;
std::atomic<uint32_t> nextId{1};
std::atomic<unsigned> active{0};

uint32_t session()
{
    static const uint32_t value = esp_random();
    return value;
}

void snapshot(const char* phase, uint32_t id)
{
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // Capture before logging, which can itself allocate. Keep messages under
    // the logger's payload limit; wall-clock timestamps are unreliable at boot.
    const uint32_t at = millis();
    const unsigned i = heap_caps_get_free_size(internal);
    const unsigned il = heap_caps_get_largest_free_block(internal);
    const unsigned im = heap_caps_get_minimum_free_size(internal);
    const unsigned p = heap_caps_get_free_size(external);
    const unsigned pl = heap_caps_get_largest_free_block(external);
    LOGI("WM b=%08lx t=%lu id=%lu e=%s i=%u il=%u im=%u p=%u pl=%u a=%u",
         (unsigned long)session(), (unsigned long)at, (unsigned long)id, phase, i, il, im, p, pl, active.load());
}

// The disconnect callback runs BEFORE the response is deleted. Its captured
// lease, however, is destroyed with the request's std::function member AFTER
// the request destructor body has deleted the response. This records response
// reclamation even when a request ends without a disconnect callback (upgrade).
// The request and callback allocation themselves are still live at this point.
class Lease {
public:
    explicit Lease(Record* record) : record_(record) {}
    Lease(const Lease& other) : record_(other.record_) { ++record_->owners; }
    Lease& operator=(const Lease&) = delete;
    ~Lease()
    {
        const uint32_t id = record_->id;
        if (record_->owners.fetch_sub(1) == 1) {
            --active;
            snapshot("released", id);
        }
    }
    void sample(const char* phase) const { snapshot(phase, record_->id); }
private:
    Record* record_;
};

void traceRequest(AsyncWebServerRequest* request, ArMiddlewareNext next)
{
    Record* record = nullptr;
    for (unsigned n = 0; n < kCapacity; ++n) {
        unsigned unused = 0;
        if (records[n].owners.compare_exchange_strong(unused, 1)) {
            record = &records[n];
            break;
        }
    }
    if (!record) {
        snapshot("capacity", 0);
        next();
        return;
    }
    record->id = nextId.fetch_add(1);
    ++active;
    Lease lease(record);
    lease.sample("enter");
    // URL excludes the query string; no credentials, headers or body are logged.
    LOGI("WMR b=%08lx id=%lu method=%.8s path=%.72s",
         (unsigned long)session(), (unsigned long)record->id,
         request->methodToString(), request->url().c_str());
    // This module owns the sole disconnect callback for ordinary HTTP requests.
    request->onDisconnect([lease]() { lease.sample("disconnect"); });
    next();
    lease.sample("ready");
}
} // namespace

void install(AsyncWebServer& server)
{
    if (!records) {
        records = static_cast<Record*>(heap_caps_malloc(
            sizeof(Record) * kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!records) {
            LOGW("WM diagnostics unavailable: PSRAM allocation failed");
            return;
        }
        for (unsigned n = 0; n < kCapacity; ++n) new (&records[n]) Record();
    }
    LOGI("WM schema=1 capacity=%u pool=%u", kCapacity, (unsigned)sizeof(Record) * kCapacity);
    server.addMiddleware(traceRequest);
}

void sample(const char* phase) { snapshot(phase, 0); }

void poll()
{
    static uint32_t lastMs = 0;
    const uint32_t now = millis();
    if (uint32_t(now - lastMs) < 5000U) return;
    lastMs = now;
    sample("periodic");
}
} // namespace WebMemoryDiagnostics
#endif

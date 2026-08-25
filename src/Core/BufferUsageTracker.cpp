#include "Core/BufferUsageTracker.h"

#include <ctype.h>
#include <string.h>

#include <freertos/FreeRTOS.h>

#if FLOW_ENABLE_BUFFER_USAGE_TRACKING
namespace {

static constexpr size_t kTrackedSourceLen = 24U;
struct TrackedBufferEntry {
    const char* name;
    const char* multiplicity;
    uint32_t capacity;
    uint32_t peakUsed;
    char source[kTrackedSourceLen] = {0};
};

static portMUX_TYPE gTrackedBufferMux = portMUX_INITIALIZER_UNLOCKED;
static TrackedBufferEntry gTrackedBuffers[] = {
    {"cfg.meta", "315", 0, 0, {0}},
    {"cfg.apply.doc", "1", 0, 0, {0}},
    {"loghub.queue", "256", 0, 0, {0}},
    {"loghub.modules", "40", 0, 0, {0}},
    {"eventbus.runtime", "50+32", 0, 0, {0}},
    {"mqtt.rx.queue", "8", 0, 0, {0}},
    {"mqtt.jobs", "80+80+80+60", 0, 0, {0}},
    {"mqtt.payload", "1", 0, 0, {0}},
    {"mqtt.reply", "1", 0, 0, {0}},
    {"mqtt.acks", "2", 0, 0, {0}},
    {"mqtt.cmd.doc", "1", 0, 0, {0}},
    {"mqtt.cfg.doc", "1", 0, 0, {0}},
    {"io.ha.tpl", "12", 0, 0, {0}},
    {"io.ha.sw.on", "8", 0, 0, {0}},
    {"io.ha.sw.off", "8", 0, 0, {0}},
    {"pooldev.persist", "8", 0, 0, {0}},
    {"pooldev.slots", "8", 0, 0, {0}},
    {"ha.entities", "24+8+16+16+8", 0, 0, {0}},
    {"hmi.cfgmenu", "72+48", 0, 0, {0}},
    {"wifi.scan", "24", 0, 0, {0}},
};

static_assert((size_t)TrackedBufferId::Count == (sizeof(gTrackedBuffers) / sizeof(gTrackedBuffers[0])),
              "Tracked buffer table mismatch");

static void copyPreview_(char* out, size_t outLen, const char* in);

static void updateEntry_(TrackedBufferEntry& entry,
                         size_t used,
                         size_t capacity,
                         const char* source,
                         const char* preview)
{
    const uint32_t cap = (capacity > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)capacity;
    if (cap > entry.capacity) entry.capacity = cap;
    const uint32_t clampedUsed = (used > cap) ? cap : (uint32_t)used;
    if (clampedUsed > entry.peakUsed) {
        entry.peakUsed = clampedUsed;
        copyPreview_(entry.source, sizeof(entry.source), source);
    }
}

static void copyPreview_(char* out, size_t outLen, const char* in)
{
    if (!out || outLen == 0U) return;
    out[0] = '\0';
    if (!in || in[0] == '\0') return;

    size_t w = 0U;
    while (in[w] != '\0' && (w + 1U) < outLen) {
        const unsigned char c = (unsigned char)in[w];
        out[w] = isprint(c) ? (char)c : ' ';
        ++w;
    }
    out[w] = '\0';
}

}  // namespace

void BufferUsageTracker::note(TrackedBufferId id,
                              size_t used,
                              size_t capacity,
                              const char* source,
                              const char* preview)
{
    const size_t idx = (size_t)id;
    if (idx >= (sizeof(gTrackedBuffers) / sizeof(gTrackedBuffers[0]))) return;

    portENTER_CRITICAL(&gTrackedBufferMux);
    updateEntry_(gTrackedBuffers[idx], used, capacity, source, preview);
    portEXIT_CRITICAL(&gTrackedBufferMux);
}

void BufferUsageTracker::noteFromISR(TrackedBufferId id,
                                     size_t used,
                                     size_t capacity,
                                     const char* source,
                                     const char* preview)
{
    const size_t idx = (size_t)id;
    if (idx >= (sizeof(gTrackedBuffers) / sizeof(gTrackedBuffers[0]))) return;

    portENTER_CRITICAL_ISR(&gTrackedBufferMux);
    updateEntry_(gTrackedBuffers[idx], used, capacity, source, preview);
    portEXIT_CRITICAL_ISR(&gTrackedBufferMux);
}

size_t BufferUsageTracker::snapshot(TrackedBufferSnapshot* out, size_t maxCount)
{
    if (!out || maxCount == 0U) return 0U;

    const size_t count = (maxCount < (sizeof(gTrackedBuffers) / sizeof(gTrackedBuffers[0])))
        ? maxCount
        : (sizeof(gTrackedBuffers) / sizeof(gTrackedBuffers[0]));

    portENTER_CRITICAL(&gTrackedBufferMux);
    for (size_t i = 0; i < count; ++i) {
        out[i].name = gTrackedBuffers[i].name;
        out[i].multiplicity = gTrackedBuffers[i].multiplicity;
        out[i].capacity = gTrackedBuffers[i].capacity;
        out[i].peakUsed = gTrackedBuffers[i].peakUsed;
        memcpy(out[i].source, gTrackedBuffers[i].source, sizeof(out[i].source));
    }
    portEXIT_CRITICAL(&gTrackedBufferMux);

    return count;
}
#endif

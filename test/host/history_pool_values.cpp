#include <ArduinoJson.h>
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Modules/Network/WebInterfaceModule/HistoryJson.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
static bool failAllocation = false;
static size_t allocations = 0;
bool psramFound() { return true; }
void* heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations; return failAllocation ? nullptr : malloc(size);
}
void heap_caps_free(void* ptr) { free(ptr); }
struct Print {
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t*, size_t) = 0;
    void print(const char* text) { write(reinterpret_cast<const uint8_t*>(text), strlen(text)); }
    void print(char c) { write(uint8_t(c)); }
};
// PRODUCTION_BUFFER
struct TestRegistry {
    bool history(ValueId id) const { return id != 10U; }
    bool read(ValueId id, ValueSnapshot& value, ValueMetadata* meta) const {
        if (id == 5U) return false; // Configured but never produced.
        const auto* descriptor = dataValueDescriptor(id);
        memset(meta->name, '\x01', sizeof(meta->name) - 1); // Worst-case label escaping.
        meta->type = (descriptor && descriptor->kind == DataValueKind::Digital) ? ValueType::Bool
                     : (descriptor && descriptor->kind == DataValueKind::Counter &&
                        descriptor->id >= ValueIds::Digital && descriptor->id < ValueIds::PulseRate)
                         ? ValueType::UInt64 : ValueType::Float;
        meta->aggregation = AggregationMode::Gauge;
        meta->unit = ValueUnit::Unspecified;
        value.quality = (id == 6U) ? ValueQuality::Invalid : ValueQuality::Valid;
        return true;
    }
};
struct TestDataStore {
    TestRegistry values;
    bool configuredHistory(ValueId id) const { return id != 10U && id != 12U; }
};
struct TestDataStoreService { TestDataStore* store; };
struct TestHistoryService {
    void* ctx = nullptr;
    bool (*readValue)(void*, uint16_t, bool, uint8_t, ValueHistoryRecord*) = nullptr;
};
static bool readEveryDay(void*, uint16_t id, bool, uint8_t age, ValueHistoryRecord* out) {
    if (id == 5U) return false; // Undefined value: no stored record.
    out->period = 1000U + age;
    out->durationMs = 3600000U;
    out->minimum = -123.456; out->maximum = 987.654; out->weightedSum = 12.345 * 3600000.0;
    out->delta = 42.0; out->rawDelta = UINT64_C(18446744073709551615);
    out->discontinuities = 3U; out->valid = true; out->boundaryUncertain = true;
    return true;
}
const char* dataValueLabel(ValueId id, const ValueMetadata& meta) {
    if (meta.name[0]) return meta.name;
    const auto* descriptor = dataValueDescriptor(id);
    return descriptor ? descriptor->label : "";
}
bool dataValueHistorized(const TestRegistry& values, ValueId id) {
    return dataValueDescriptor(id) != nullptr && values.history(id);
}
static unsigned errors = 0;
void sendTinyBusyJson_(void*, const char*) { ++errors; }
static void buildValues(const TestHistoryService* history, const TestDataStoreService* store,
                        std::shared_ptr<WebJsonBuffer>& result) {
    // PRODUCTION_ALLOC
    DynamicJsonDocument doc(4096);
    void* request = nullptr;
    body->print("{\"values\":[");
    // PRODUCTION_VALUES
    body->print("]}");
    result = body;
}
static JsonVariant findValue(const JsonArray& values, ValueId id) {
    for (const JsonVariant& item : values) if (item["id"].as<unsigned>() == id) return item;
    assert(false); return JsonVariant();
}
int main() {
    TestDataStore dataStore;
    TestDataStoreService service{&dataStore};
    TestHistoryService history{nullptr, &readEveryDay};
    std::shared_ptr<WebJsonBuffer> body;
    buildValues(&history, &service, body);
    assert(errors == 0 && allocations == 1 && body && body->finish());
    std::string received;
    uint8_t chunk[127];
    for (size_t index = 0; index < body->length();) {
        size_t n = body->fillAt(chunk, sizeof(chunk), index);
        assert(n && n <= sizeof(chunk));
        received.append(reinterpret_cast<char*>(chunk), n); index += n;
    }
    assert(body->fillAt(chunk, sizeof(chunk), body->length()) == 0);
    DynamicJsonDocument json(384 * 1024);
    assert(!deserializeJson(json, received));
    JsonArray values = json["values"].as<JsonArray>();
    // Only historized values with a currently valid sample; disabled (10), undefined (5) and invalid (6) are absent.
    assert(values.size() == kDataValueCount - 3U);
    for (const JsonVariant& item : values) {
        const unsigned id = item["id"].as<unsigned>();
        assert(id != 5U && id != 6U && id != 10U);
    }
    // Grouping and the pending-restart flag mirror the data exposure page.
    assert(strcmp(findValue(values, 0)["group"].as<const char*>(), "Entr\u00e9es analogiques") == 0);
    assert(findValue(values, 12)["pending_restart"].as<bool>() == true);
    assert(findValue(values, 0)["pending_restart"].as<bool>() == false);
    // Every value carries eight daily slots; a value with no stored record keeps them empty.
    JsonArray recorded = findValue(values, 0)["days"].as<JsonArray>();
    assert(recorded.size() == ValueHistory::PsramDays + 1U);
    assert(recorded[0]["average"].as<double>() > 12.0 && recorded[7]["average"].as<double>() > 12.0);
    printf("History pool values: valid+historized filter, restart flag, daily records and bounded transport OK\n");
}

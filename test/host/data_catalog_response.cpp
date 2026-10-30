#include <ArduinoJson.h>
#include "Core/Generated/DataValueCatalog_Generated.h"
#include "Core/Generated/RuntimeUiManifest_Generated.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
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
static ValueId activeEquipment = VALUE_INVALID;
struct TestRegistry {
    bool usable(ValueId) const { return true; }
    bool read(ValueId id, ValueSnapshot& value, ValueMetadata* meta = nullptr) const {
        if (ValueIds::isEquipment(id) && activeEquipment != VALUE_INVALID && id != activeEquipment) return false;
        // Maximum runtime label escaping and exact 64-bit values.
        if (meta) {
            memset(meta->name, '\x01', sizeof(meta->name) - 1);
            const auto* descriptor = dataValueDescriptor(id);
            meta->type = descriptor && dataValueIsBinary(*descriptor) ? ValueType::Bool : ValueType::UInt64;
        }
        value.quality = id == activeEquipment ? ValueQuality::Invalid : ValueQuality::Valid;
        const auto* descriptor = dataValueDescriptor(id);
        if (descriptor && dataValueIsBinary(*descriptor)) value.value.b = true;
        else value.value.u64 = UINT64_MAX;
        return true;
    }
};
struct TestStore {
    TestRegistry values;
    bool publishHa(ValueId) const { return false; }
    bool configuredPublishHa(ValueId) const { return true; }
    bool configuredUsable(ValueId) const { return false; }
    bool configuredHistory(ValueId) const { return true; }
};
const char* dataValueLabel(ValueId, const ValueMetadata& meta) { return meta.name; }
static unsigned errors = 0;
void sendTinyBusyJson_(void*, const char*) { ++errors; }
static void buildResponse(const TestStore* dataStore_, std::shared_ptr<WebJsonBuffer>& result) {
    void* request = nullptr;
    // PRODUCTION_BUILD
    result = body;
}
int main() {
    TestStore store;
    std::shared_ptr<WebJsonBuffer> body;
    buildResponse(&store, body);
    assert(errors == 0 && allocations == 1 && body && body->finish());
    std::string received;
    uint8_t chunk[127];
    for (size_t index = 0; index < body->length();) {
        size_t n = body->fillAt(chunk, sizeof(chunk), index);
        assert(n && n <= sizeof(chunk));
        received.append(reinterpret_cast<char*>(chunk), n); index += n;
    }
    assert(body->fillAt(chunk, sizeof(chunk), body->length()) == 0);
    DynamicJsonDocument json(1024 * 1024);
    assert(!deserializeJson(json, received));
    assert(json["values"].size() == kDataValueCount);
    assert(json["values"][0]["value"].as<std::string>() == "18446744073709551615");
    assert(json["values"][0]["name"].as<std::string>().size() == 63);
    assert(json["values"][0]["pending_restart"].as<bool>());
    auto retained = [body](uint8_t* out) { return body->fillAt(out, 1, 0); };
    body.reset();
    assert(retained(chunk) == 1 && chunk[0] == '{');
    activeEquipment=ValueIds::Equipment+1;
    buildResponse(&store, body);
    assert(body && body->finish());
    std::string selected;
    for(size_t index=0; index<body->length();) {
        const size_t n=body->fillAt(chunk,sizeof(chunk),index);
        assert(n); selected.append(reinterpret_cast<char*>(chunk),n); index+=n;
    }
    json.clear(); assert(!deserializeJson(json,selected));
    size_t expected=1;
    for(const auto& descriptor:kDataValues) if(!dataValueIsEquipment(descriptor)) ++expected;
    assert(json["values"].size()==expected);
    bool found=false;
    for(JsonObject item:json["values"].as<JsonArray>()) if(item["id"].as<ValueId>()==activeEquipment) {
        found=true; assert(!item["available"].as<bool>() && item["defined"].as<bool>());
        assert(item["value"].isNull());
    }
    assert(found);
    body.reset();
    failAllocation = true;
    buildResponse(&store, body);
    assert(!body && errors == 1);
    failAllocation = false;
    WebJsonBuffer tiny(4);
    tiny.print("oversized");
    assert(tiny.overflowed() && !tiny.finish());
    printf("Catalog response: %zu entries, %zu bytes, one PSRAM allocation, exact integers, async lifetime and safe failures OK\n", kDataValueCount, received.size());
}

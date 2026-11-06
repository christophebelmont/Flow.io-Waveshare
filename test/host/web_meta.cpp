#include <ArduinoJson.h>
#include "Core/Services/IHmi.h"
#include "Core/SystemStats.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#define FLOW_BUILD_PROFILE_NAME "waveshare"
#define TFT_FIRMW "6.0.0"
#define FLOW_WEB_UNIFY_STATUS_CARD_ICONS 0
namespace FirmwareVersion { constexpr auto Full = "3.0.1-20261106000853"; }
const char* webAssetVersion_() { return "20261106000853-12345678"; }
enum class NetworkAccessMode { None, Station, AccessPoint };
enum class ServiceId { NetworkAccess, Hmi };
struct NetworkAccessService { void* ctx; NetworkAccessMode (*mode)(void*); };
struct WifiService { void* ctx; bool (*isConnected)(void*); };
struct Registry { template<class T> T* get(ServiceId) { return nullptr; } };

struct AsyncWebServerResponse {
    std::string body;
    bool noCache = false;
    virtual ~AsyncWebServerResponse() = default;
};
struct AsyncResponseStream : AsyncWebServerResponse {
    size_t write(uint8_t byte) { body.push_back(static_cast<char>(byte)); return 1; }
    size_t write(const uint8_t* data, size_t size) {
        body.append(reinterpret_cast<const char*>(data), size); return size;
    }
};
struct AsyncWebServerRequest {
    unsigned status = 0;
    std::string body;
    bool noCache = false;
    bool failAllocation = false;
    void send(unsigned code, const char*, const char* text) { status = code; body = text; }
    AsyncWebServerResponse* beginResponse(unsigned, const char*, const char* text) {
        auto* response = new AsyncWebServerResponse;
        response->body = text;
        return response;
    }
    AsyncResponseStream* beginResponseStream(const char*, size_t = 0) {
        return failAllocation ? nullptr : new AsyncResponseStream;
    }
    void send(AsyncWebServerResponse* response) {
        status = 200; body = response->body; noCache = response->noCache; delete response;
    }
};
void addNoCacheHeaders_(AsyncWebServerResponse* response) { response->noCache = true; }
void sendTinyBusyJson_(AsyncWebServerRequest* request, const char*) {
    request->send(503, "application/json", "{\"ok\":false}");
}
const char* networkTransport_(NetworkAccessMode) { return "ethernet"; }
void loadConfiguredDeviceName_(void*, char* out, size_t size) {
    // Long names and JSON escaping must not truncate the response.
    snprintf(out, size, "Pool \"controller\" \\ local technical room");
}
void SystemStats::collect(SystemStatsSnapshot& out) {
    out.uptimeMs = UINT32_MAX;
    out.heap = {8000000, 7000000, 6000000, 25, 300000, 200000, 100000, 30};
}

struct Fixture {
    bool local = false, remote = false, version = false;
    NetworkAccessService* netAccessSvc_ = nullptr;
    WifiService* wifiSvc_ = nullptr;
    Registry* services_ = nullptr;
    void* cfgStore_ = nullptr;
    bool provisioningOnly_ = false, runtimeEventsAvailable_ = true;
    HmiService hmi{};
    HmiService* hmiSvc_ = &hmi;
    Fixture() {
        hmi.ctx = this;
        hmi.getDisplayVersion = [](void* ctx, char* out, size_t size) {
            snprintf(out, size, "6.0.0");
            return static_cast<Fixture*>(ctx)->version;
        };
        hmi.getLocalDisplayIdentity = [](void* ctx, HmiDisplayIdentity* out) {
            snprintf(out->model, sizeof(out->model), "NX8048P070_011C");
            snprintf(out->compatibility, sizeof(out->compatibility), "NX8048P070_011");
            out->deviceFirmwareVersion = 123;
            out->touchType = HmiDisplayTouchType::Capacitive;
            return static_cast<Fixture*>(ctx)->local;
        };
        hmi.getRemoteDisplayStatus = [](void* ctx, bool* enabled, bool* connected, uint8_t* protocol) {
            *enabled = *connected = static_cast<Fixture*>(ctx)->remote;
            *protocol = 7;
            return true;
        };
    }
    void respond(AsyncWebServerRequest* request) {
// PRODUCTION_HANDLER
    }
};

int main() {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture fixture;
        fixture.local = scenario == 1 || scenario == 3;
        fixture.remote = scenario == 2;
        fixture.version = scenario == 1 || scenario == 2;
        AsyncWebServerRequest request;
        fixture.respond(&request);
        fprintf(stderr, "metadata scenario=%u status=%u bytes=%zu\n", scenario, request.status, request.body.size());
        assert(request.status == 200);
        assert(request.noCache);
        DynamicJsonDocument result(4096);
        assert(deserializeJson(result, request.body) == DeserializationError::Ok);
        assert(result["firmware_version"] == FirmwareVersion::Full);
        assert(result["nextion_display_detected"].as<bool>() == (fixture.local || fixture.remote));
        assert(result["nextion_display_remote"].as<bool>() == fixture.remote);
        assert(result["nextion_display_version_detected"].as<bool>() == fixture.version);
        if (fixture.version) assert(result["nextion_display_version"] == "6.0.0");
        else assert(!result.containsKey("nextion_display_version"));
        assert(result["heap"]["internal_frag"] == 30);
    }
    Fixture fixture;
    AsyncWebServerRequest request;
    request.failAllocation = true;
    fixture.respond(&request);
    assert(request.status == 503);
}

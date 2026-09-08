#pragma once
/**
 * @file AiInsightModule.h
 * @brief Configuration and weather context foundation for pool insights.
 */

#include "Core/ConfigTypes.h"
#include "Core/Module.h"
#include "Core/NvsKeys.h"
#include "Core/ServiceBinding.h"
#include "Core/Services/Services.h"
#include "Modules/AiInsightModule/OpenAiResponsesClient.h"
#include "Modules/AiInsightModule/OpenMeteoWeatherClient.h"

struct AiInsightConfig {
    static constexpr size_t ApiKeyCapacity = 192U;
    static constexpr size_t ModelCapacity = 48U;

    bool enabled = false;
    char apiKey[ApiKeyCapacity]{};
    char model[ModelCapacity]{};
    double latitude = 91.0;
    double longitude = 0.0;
};

class AiInsightModule : public Module {
public:
    AiInsightModule() = default;
    ~AiInsightModule() override;
    AiInsightModule(const AiInsightModule&) = delete;
    AiInsightModule& operator=(const AiInsightModule&) = delete;

    ModuleId moduleId() const override { return ModuleId::AiInsight; }
    const char* taskName() const override { return "ai.insight"; }
    uint8_t dependencyCount() const override { return 6U; }
    ModuleId dependency(uint8_t index) const override {
        if (index == 0U) return ModuleId::LogHub;
        if (index == 1U) return ModuleId::ConfigStore;
        if (index == 2U) return ModuleId::Command;
        if (index == 3U) return ModuleId::Ethernet;
        if (index == 4U) return ModuleId::Time;
        if (index == 5U) return ModuleId::PoolHistory;
        return ModuleId::Unknown;
    }
    uint8_t taskCount() const override { return 1U; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }
    uint16_t taskStackSize() const override { return 8192U; }
    UBaseType_t taskStackCaps() const override {
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    BaseType_t taskCore() const override { return 0; }
    uint32_t startDelayMs() const override { return 7000U; }

    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore& cfg, ServiceRegistry& services) override;
    void loop() override;

private:
    struct Storage;

    static constexpr uint8_t kOpenAiConfigBranch = 1U;
    static constexpr uint8_t kLocationConfigBranch = 2U;
    static constexpr uint32_t kWeatherCacheLifetimeMs = 30U * 60U * 1000U;
    static constexpr uint32_t kPoolInsightReuseLifetimeSec = 60U * 60U;
    static constexpr uint32_t kLoopDelayMs = 100U;

    static bool cmdWeatherRefresh_(void* userCtx,
                                   const CommandRequest& request,
                                   char* reply,
                                   size_t replyLen);
    static bool cmdWeatherStatus_(void* userCtx,
                                  const CommandRequest& request,
                                  char* reply,
                                  size_t replyLen);
    bool requestWeatherRefresh_(bool force, char* errOut, size_t errOutLen);
    bool getWeatherStatus_(AiWeatherStatus* outStatus) const;
    bool buildPoolPreview_(AiPoolInsightPreview* outPreview,
                           char* errOut,
                           size_t errOutLen) const;
    bool requestPoolInsight_(bool* outReused, char* errOut, size_t errOutLen);
    bool getPoolInsightStatus_(AiPoolInsightStatus* outStatus) const;
    bool buildWeatherStatusJson_(char* out, size_t outLen) const;
    void processWeatherRequest_();
    void processPoolInsightRequest_();
    void finishWeatherRequest_(AiWeatherState state,
                               const PoolWeatherSnapshot* weather,
                               const char* message);
    void finishPoolInsightRequest_(AiPoolInsightState state,
                                   const OpenAiResponsesParser::Result* result,
                                   const char* text,
                                   const char* message);
    uint64_t currentEpoch_() const;
    bool networkReady_() const;
    static bool locationIsValid_(double latitude, double longitude);
    static bool writeError_(char* out, size_t outLen, const char* message);

    AiInsightConfig cfgData_{};
    ConfigVariable<bool, 0> enabledVar_{
        NVS_KEY(NvsKeys::AiInsight::Enabled), "enabled", "ai/openai",
        ConfigType::Bool, &cfgData_.enabled, ConfigPersistence::Persistent, 0U
    };
    ConfigVariable<char, 0> apiKeyVar_{
        NVS_KEY(NvsKeys::AiInsight::ApiKey), "api_key", "ai/openai",
        ConfigType::CharArray, cfgData_.apiKey, ConfigPersistence::Persistent,
        sizeof(cfgData_.apiKey)
    };
    ConfigVariable<char, 0> modelVar_{
        NVS_KEY(NvsKeys::AiInsight::Model), "model", "ai/openai",
        ConfigType::CharArray, cfgData_.model, ConfigPersistence::Persistent,
        sizeof(cfgData_.model)
    };
    ConfigVariable<double, 0> latitudeVar_{
        NVS_KEY(NvsKeys::AiInsight::Latitude), "latitude", "system/location",
        ConfigType::Double, &cfgData_.latitude, ConfigPersistence::Persistent, 0U
    };
    ConfigVariable<double, 0> longitudeVar_{
        NVS_KEY(NvsKeys::AiInsight::Longitude), "longitude", "system/location",
        ConfigType::Double, &cfgData_.longitude, ConfigPersistence::Persistent, 0U
    };

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Storage* storage_ = nullptr;
    OpenAiResponsesClient openAiClient_{};
    OpenMeteoWeatherClient weatherClient_{};
    const CommandService* commandService_ = nullptr;
    const NetworkAccessService* networkAccessService_ = nullptr;
    const TimeService* timeService_ = nullptr;
    const PoolHistoryService* poolHistoryService_ = nullptr;

    AiInsightService service_{
        ServiceBinding::bind<&AiInsightModule::requestWeatherRefresh_>,
        ServiceBinding::bind<&AiInsightModule::getWeatherStatus_>,
        ServiceBinding::bind<&AiInsightModule::buildPoolPreview_>,
        ServiceBinding::bind<&AiInsightModule::requestPoolInsight_>,
        ServiceBinding::bind<&AiInsightModule::getPoolInsightStatus_>,
        this
    };
};

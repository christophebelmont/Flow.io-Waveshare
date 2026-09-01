#pragma once
/**
 * @file SystemModule.h
 * @brief System command module (ping/reboot/factory reset).
 */
#include "Board/BoardTypes.h"
#include "Core/Module.h"
#include "Core/NvsKeys.h"
#include "Core/RuntimeUi.h"
#include "Core/EventBus/EventBus.h"
#include "Core/ServiceBinding.h"
#include "Core/Services/Services.h"

/**
 * @brief Registers system commands and monitors the hardware factory-reset button.
 */
struct BoardSpec;

class SystemModule : public Module, public IRuntimeUiValueProvider {
public:
    explicit SystemModule(const BoardSpec& board);

    /** @brief Module id. */
    ModuleId moduleId() const override { return ModuleId::System; }
    ModuleId runtimeUiProviderModuleId() const override { return moduleId(); }
    const char* taskName() const override { return "system"; }
    uint16_t taskStackSize() const override { return 3072; }
    BaseType_t taskCore() const override { return 1; }
    uint8_t taskCount() const override { return factoryResetButtonEnabled_() ? 1U : 0U; }
    const ModuleTaskSpec* taskSpecs() const override { return singleLoopTaskSpec(); }

    /** @brief Depends on log hub, command service, config service and event bus. */
    uint8_t dependencyCount() const override { return 4; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::LogHub;
        if (i == 1) return ModuleId::Command;
        if (i == 2) return ModuleId::ConfigStore;
        if (i == 3) return ModuleId::EventBus;
        return ModuleId::Unknown;
    }

    /** @brief Register system commands. */
    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore&, ServiceRegistry&) override;
    void onStart(ConfigStore&, ServiceRegistry&) override;
    void loop() override;
    bool writeRuntimeUiValue(uint8_t valueId, IRuntimeUiWriter& writer) const override;

private:
    struct SystemConfig {
        char lang[8] = "fr";
        char devicename[33] = "flowio";
    };

    enum RuntimeUiValueId : uint8_t {
        RuntimeUiFirmware = 1,
        RuntimeUiUptimeMs = 2,
        RuntimeUiHeapFree = 3,
        RuntimeUiHeapMinFree = 4,
    };

    const CommandService* cmdSvc = nullptr;
    const ConfigStoreService* cfgSvc = nullptr;
    const LogHubService* logHub = nullptr;
    const EventBusService* eventBusSvc_ = nullptr;
    EventBus* eventBus_ = nullptr;
    ServiceRegistry* services_ = nullptr;
    portMUX_TYPE restartMux_ = portMUX_INITIALIZER_UNLOCKED;
    bool restartScheduled_ = false;
    uint32_t restartDelayMs_ = 0U;
    char restartReason_[24] = {0};
    SystemConfig cfgData_{};
    uint32_t localeGeneration_ = 1U;
    LocalUiInputSpec inputCfg_{};
    bool factoryResetRawActive_ = false;
    bool factoryResetDebouncedActive_ = false;
    bool factoryResetTriggered_ = false;
    uint32_t factoryResetRawChangedMs_ = 0U;
    uint32_t factoryResetPressedMs_ = 0U;

    ConfigVariable<char,0> languageVar_{
        NVS_KEY(NvsKeys::System::Language), "lang", "system", ConfigType::CharArray,
        (char*)cfgData_.lang, ConfigPersistence::Persistent, sizeof(cfgData_.lang)
    };
    ConfigVariable<char,0> deviceNameVar_{
        NVS_KEY(NvsKeys::System::DeviceName), "devicename", "system", ConfigType::CharArray,
        (char*)cfgData_.devicename, ConfigPersistence::Persistent, sizeof(cfgData_.devicename)
    };
    LocaleService localeSvc_{
        ServiceBinding::bind<&SystemModule::localeLanguage_>,
        ServiceBinding::bind<&SystemModule::localeGenerationValue_>,
        this
    };

    static bool cmdPing(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdReboot(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static bool cmdFactoryReset(void* userCtx, const CommandRequest& req, char* reply, size_t replyLen);
    static void restartTaskStatic_(void* userCtx);
    bool scheduleRestart_(uint32_t delayMs, const char* reason);
    bool performFactoryReset_();
    bool factoryResetButtonEnabled_() const;
    bool readFactoryResetButton_() const;
    void notifyShutdownPending_();
    static void onEventStatic_(const Event& e, void* user);
    void onEvent_(const Event& e);
    bool normalizeLanguage_(bool bumpGenerationIfChanged);
    const char* localeLanguage_() const;
    uint32_t localeGenerationValue_() const;
    static bool isLangCode_(const char* value, char c0, char c1);
};

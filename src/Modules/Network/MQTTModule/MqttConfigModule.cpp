#include "Modules/Network/MQTTModule/MqttConfigModule.h"

void MqttConfigModule::init(ConfigStore& cfg, ServiceRegistry&)
{
    constexpr uint8_t kCfgModuleId = (uint8_t)ConfigModuleId::Mqtt;
    constexpr uint8_t kCfgBranchId = 1;
    cfg.registerVar(hostVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(portVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(userVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(passVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(baseTopicVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(topicDeviceIdVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(deviceNameVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(enabledVar_, kCfgModuleId, kCfgBranchId);
}

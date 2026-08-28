/**
 * @file CommandModule.cpp
 * @brief Implementation file.
 */
#include "CommandModule.h"
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::CommandModule)
#include "Core/ModuleLog.h"


bool CommandModule::registerHandler_(const char* cmd, CommandHandler fn, void* userCtx) {
    return registry.registerHandler(cmd, fn, userCtx);
}

bool CommandModule::execute_(const char* cmd, const char* json, const char* args,
                             char* reply, size_t replyLen) {
    return registry.execute(cmd, json, args, reply, replyLen);
}

void CommandModule::init(ConfigStore&, ServiceRegistry& services) {
    if (!services.add(ServiceId::Command, &svc_)) {
        LOGE("service registration failed: %s", toString(ServiceId::Command));
    }
    
    logHub = services.get<LogHubService>(ServiceId::LogHub);
    LOGI("CommandService registered");
}

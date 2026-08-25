#include "App/Bootstrap.h"

#include "Core/Services/IHmi.h"
#include "Core/Services/ILogger.h"

#ifndef FLOW_ENABLE_BOOT_LOG_CAPTURE
#define FLOW_ENABLE_BOOT_LOG_CAPTURE 0
#endif
#include "Profiles/Waveshare/WaveshareProfile.h"

namespace {

AppContext gContext{};
bool gStarted = false;
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
bool gBootLogCaptureCompleteMarked = false;

bool bootLogCaptureWaitsForHaDiscovery()
{
    return gContext.services.has(ServiceId::Ha);
}
#endif

const FirmwareProfile& resolveProfile()
{
    return Profiles::Waveshare::profile();
}

}  // namespace

namespace Bootstrap {

#if FLOW_ENABLE_BOOT_LOG_CAPTURE
void markBootLogCaptureCompleteIfReady()
{
    if (gBootLogCaptureCompleteMarked) return;
    if (!gContext.moduleManager.startupComplete()) return;
    if (bootLogCaptureWaitsForHaDiscovery()) return;

    markBootLogCaptureComplete();
    gBootLogCaptureCompleteMarked = true;
}
#endif

void run()
{
    if (gStarted) return;

    const FirmwareProfile& profile = resolveProfile();
    gContext.profile = &profile;
    gContext.board = profile.board;
    gContext.domain = profile.domain;
    gContext.identity = &profile.identity;
    (void)gContext.services.add(ServiceId::I2cBus, &gContext.primaryI2cBusService);

    if (profile.setup) {
        profile.setup(gContext);
    }

    gContext.bootCompleted = true;
    if (const HmiService* hmi = gContext.services.get<HmiService>(ServiceId::Hmi)) {
        if (hmi->setBootComplete) hmi->setBootComplete(hmi->ctx);
    }
    gStarted = true;
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
    markBootLogCaptureCompleteIfReady();
#endif
}

void loop()
{
    if (!gStarted) {
        run();
    }

    (void)gContext.moduleManager.tickStartup(gContext.registry, gContext.services);
#if FLOW_ENABLE_BOOT_LOG_CAPTURE
    markBootLogCaptureCompleteIfReady();
#endif

    if (gContext.profile && gContext.profile->loop) {
        gContext.profile->loop(gContext);
    }
}

AppContext& context()
{
    return gContext;
}

const FirmwareProfile& activeProfile()
{
    return resolveProfile();
}

}  // namespace Bootstrap

/**
 * @file SystemMonitorModule.cpp
 * @brief Implementation file.
 */
#include "SystemMonitorModule.h"
#include "Core/BufferUsageTracker.h"
#include "Core/ModuleManager.h"   ///< required for iteration
#include "Core/CommandRegistry.h"  // Full CommandRequest for runtime mode args.
#include "Core/Services/ILogger.h"
#include "Core/SaturatingCounter.h"
#include <Arduino.h>
#include <WiFi.h>                ///< only for RSSI (optional)
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <new>
#include <algorithm>
#include <string.h>
#ifdef CONFIG_HEAP_TASK_TRACKING
#include <esp_heap_task_info.h>
#endif
#ifndef FLOW_WEB_HEAP_FORENSICS
#define FLOW_WEB_HEAP_FORENSICS 0
#endif
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::SystemMonitorModule)
#include "Core/ModuleLog.h"

namespace {
static constexpr uint8_t kSysMonCfgProducerId = 45;
static constexpr uint8_t kSysMonCfgBranch = 1;
static constexpr uint32_t kWebWatchdogMinCheckPeriodMs = 250U;
static constexpr uint32_t kWebWatchdogMinStaleMs = 1000U;
static constexpr uint32_t kWebWatchdogMinBootGraceMs = 5000U;
static constexpr uint32_t kWebWatchdogClientIdleFactor = 2U;
static constexpr uint8_t kWebWatchdogMaxFailuresCap = 20U;
static constexpr uint32_t kPressurePanicRebootDelayMs = 5000U;
static constexpr uint32_t kPressureCriticalRebootDelayMs = 15000U;
static constexpr uint32_t kPressureSheddingRecoveryFloorFreeBytes = 18000U;
static constexpr uint32_t kPressureSheddingRecoveryFreeBytes = 22000U;
static constexpr uint32_t kPressureSheddingRecoveryLargestBytes = 8192U;
static constexpr uint8_t kPressureSheddingRecoveryFragPercent = 30U;
static constexpr MqttConfigRouteProducer::Route kSysMonCfgRoutes[] = {
    {1, {(uint8_t)ConfigModuleId::SystemMonitor, kSysMonCfgBranch}, "sysmon", "sysmon", (uint8_t)MqttPublishPriority::Normal, nullptr},
};

volatile bool gHeapAllocFailedPending = false;
volatile size_t gHeapAllocFailedSize = 0;
volatile uint32_t gHeapAllocFailedCaps = 0;
const char* volatile gHeapAllocFailedFunction = nullptr;

enum class MemoryPressureState : uint8_t {
    Normal = 0,
    Constrained = 1,
    Shedding = 2,
    Critical = 3,
    Panic = 4,
};

MemoryPressureState deriveMemoryPressureState_(const SystemStatsSnapshot& snap)
{
    const uint32_t freeBytes = snap.heap.internalFreeBytes;
    const uint32_t largestBytes = snap.heap.internalLargestFreeBlock;
    const uint8_t frag = snap.heap.internalFragPercent;

    // Absolute internal-memory shortages are sufficient to escalate. Heap
    // fragmentation may escalate earlier, but can never hide a low-free state.
    if (freeBytes < 6000U || largestBytes < 2048U) return MemoryPressureState::Panic;
    if (freeBytes < 12000U || largestBytes < 4096U) return MemoryPressureState::Critical;
    if (freeBytes < 16000U || largestBytes < 6144U ||
        (freeBytes < 20000U && frag > 35U)) return MemoryPressureState::Shedding;
    if (freeBytes < 24000U || largestBytes < 10000U || frag > 45U) {
        return MemoryPressureState::Constrained;
    }
    return MemoryPressureState::Normal;
}

MemoryPressureState applyMemoryPressureHysteresis_(const SystemStatsSnapshot& snap,
                                                   MemoryPressureState previous)
{
    const MemoryPressureState derived = deriveMemoryPressureState_(snap);
    if ((uint8_t)derived >= (uint8_t)previous) return derived;

    const uint32_t freeBytes = snap.heap.internalFreeBytes;
    const uint32_t largestBytes = snap.heap.internalLargestFreeBlock;
    const uint8_t frag = snap.heap.internalFragPercent;

    // Escalation uses the strict thresholds above immediately. Recovery needs
    // extra margin so short-lived heap allocations cannot make adjacent states
    // alternate and emit a trace on every sysmon pass.
    switch (previous) {
    case MemoryPressureState::Panic:
        if (freeBytes < 8000U || largestBytes < 3072U) return previous;
        break;
    case MemoryPressureState::Critical:
        if (freeBytes < 14000U || largestBytes < 5120U) return previous;
        break;
    case MemoryPressureState::Shedding:
        // Shedding can be entered by the combination of less than 20 KiB free
        // and more than 35% fragmentation. Require a margin on both sides of
        // that compound condition before recovering, otherwise ordinary
        // short-lived allocations make the state alternate around 20 KiB.
        if (freeBytes < kPressureSheddingRecoveryFloorFreeBytes ||
            largestBytes < kPressureSheddingRecoveryLargestBytes ||
            (freeBytes < kPressureSheddingRecoveryFreeBytes &&
             frag > kPressureSheddingRecoveryFragPercent)) {
            return previous;
        }
        break;
    case MemoryPressureState::Constrained:
        if (freeBytes < 28000U || largestBytes < 12288U || frag > 35U) return previous;
        break;
    case MemoryPressureState::Normal:
    default:
        break;
    }
    return derived;
}

const char* memoryPressureStateStr_(MemoryPressureState st)
{
    switch (st) {
    case MemoryPressureState::Normal: return "normal";
    case MemoryPressureState::Constrained: return "constrained";
    case MemoryPressureState::Shedding: return "shedding";
    case MemoryPressureState::Critical: return "critical";
    case MemoryPressureState::Panic: return "panic";
    default: return "normal";
    }
}

void onHeapAllocFailed_(size_t size, uint32_t caps, const char* functionName)
{
    gHeapAllocFailedSize = size;
    gHeapAllocFailedCaps = caps;
    gHeapAllocFailedFunction = functionName;
    gHeapAllocFailedPending = true;
}

#ifdef CONFIG_HEAP_TASK_TRACKING
void copyTaskName_(char* out, size_t outLen, TaskHandle_t task)
{
    if (!out || outLen == 0U) return;
    out[0] = '\0';

    const char* name = task ? pcTaskGetName(task) : pcTaskGetName(nullptr);
    if (!name || !name[0]) {
        snprintf(out, outLen, "%s", "-");
        return;
    }

    snprintf(out, outLen, "%s", name);
}
#endif

}

const char* SystemMonitorModule::wifiStateStr(WifiState st) {
    switch (st) {
    case WifiState::Disabled:    return "Disabled";
    case WifiState::Idle:        return "Idle";
    case WifiState::Connecting:  return "Connecting";
    case WifiState::Connected:   return "Connected";
    case WifiState::ErrorWait:   return "ErrorWait";
    default:                     return "Unknown";
    }
}

void SystemMonitorModule::init(ConfigStore& cfg, ServiceRegistry& services) {
    constexpr uint8_t kCfgModuleId = (uint8_t)ConfigModuleId::SystemMonitor;
    constexpr uint8_t kCfgBranchId = kSysMonCfgBranch;
    services_ = &services;
    cfgStore_ = &cfg;
    cfg.registerVar(tracePeriodVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogEnabledVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogCheckPeriodVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogStaleVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogBootGraceVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogMaxFailuresVar_, kCfgModuleId, kCfgBranchId);
    cfg.registerVar(webWatchdogAutoRebootVar_, kCfgModuleId, kCfgBranchId);

    wifiSvc = services.get<WifiService>(ServiceId::Wifi);
    netAccessSvc_ = services.get<NetworkAccessService>(ServiceId::NetworkAccess);
    webInterfaceSvc_ = services.get<WebInterfaceService>(ServiceId::WebInterface);
    fwUpdateSvc_ = services.get<FirmwareUpdateService>(ServiceId::FirmwareUpdate);
    cmdSvc_ = services.get<CommandService>(ServiceId::Command);
    cfgSvc  = services.get<ConfigStoreService>(ServiceId::ConfigStore);
    logHub  = services.get<LogHubService>(ServiceId::LogHub);
    haSvc_  = services.get<HAService>(ServiceId::Ha);
    if (const auto* eventBus = services.get<EventBusService>(ServiceId::EventBus)) {
        eventBus_ = eventBus->bus;
    }
    if (!services.add(ServiceId::SystemHealth, &healthService_)) {
        LOGD("System health service registration failed");
    }

    if (cmdSvc_ && !cmdSvc_->registerHandler(cmdSvc_->ctx, "sysmon.capture_tasks", requestTaskCapture_, this)) {
        LOGD("Task capture command registration failed");
    }
    if (cmdSvc_ && !cmdSvc_->registerHandler(cmdSvc_->ctx, "sysmon.diagnostics_mode", setDiagnosticsMode_, this)) {
        LOGD("Diagnostics mode command registration failed");
    }
    if (moduleManager) {
        for (uint8_t i = 0; i < moduleManager->getCount(); ++i) {
            if (moduleManager->getModule(i)->externalTaskStackSource().acquire) ++externalStackCount_;
        }
        const size_t capacity = Limits::Core::Capacity::MaxTaskHealthEntries + externalStackCount_;
        void* storage = heap_caps_malloc(capacity * sizeof(StackSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage) {
            stackSlots_ = static_cast<StackSlot*>(storage);
            for (size_t i = 0; i < capacity; ++i) new (&stackSlots_[i]) StackSlot{};
            size_t index = Limits::Core::Capacity::MaxTaskHealthEntries;
            for (uint8_t i = 0; i < moduleManager->getCount(); ++i) {
                auto* module = moduleManager->getModule(i);
                const auto source = module->externalTaskStackSource();
                if (!source.acquire) continue;
                stackSlots_[index].source = source;
                stackSlots_[index++].id = taskHealthId(module->moduleId(), TaskRole::Library);
            }
            LOGD("Task stack sampler psram_b=%lu external=%u period_ms=%lu",
                 (unsigned long)(capacity * sizeof(StackSlot)), (unsigned)externalStackCount_,
                 (unsigned long)kStackSamplePeriodMs);
        } else {
            LOGD("Task stack sampler unavailable: PSRAM allocation failed");
        }
    }

#if FLOW_WEB_HEAP_FORENSICS
    heapWatchSamples_ = static_cast<HeapWatchSample*>(
        heap_caps_calloc(kHeapWatchSampleCount,
                         sizeof(HeapWatchSample),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    if (heapWatchSamples_) {
        LOGD("HeapWatch history allocated in PSRAM bytes=%lu",
             (unsigned long)(kHeapWatchSampleCount * sizeof(HeapWatchSample)));
    } else {
        LOGD("HeapWatch history unavailable: PSRAM allocation failed bytes=%lu",
             (unsigned long)(kHeapWatchSampleCount * sizeof(HeapWatchSample)));
    }
    const esp_err_t allocHookErr = heap_caps_register_failed_alloc_callback(&onHeapAllocFailed_);
    if (allocHookErr != ESP_OK) {
        LOGD("Heap alloc-fail hook registration failed err=%d", (int)allocHookErr);
    }
#endif
}

void SystemMonitorModule::registerHaEntities_(ServiceRegistry& services)
{
    if (haEntitiesRegistered_) return;
    if (!haSvc_) haSvc_ = services.get<HAService>(ServiceId::Ha);
    if (!haSvc_ || !haSvc_->addSensor) return;

    const HASensorEntry uptimeMinutes{
        "system",
        "sys_upt_mn",
        "Uptime",
        "rt/system/state",
        "{{ ((value_json.upt_ms | float(0)) / 60000) | round(0) | int(0) }}",
        "diagnostic",
        "mdi:timer-outline",
        "mn",
        false,
        nullptr
    };
    const HASensorEntry heapFreeBytes{
        "system",
        "sys_hp_free",
        "Heap Free",
        "rt/system/state",
        "{{ ((value_json.heap.free | float(0)) / 1024) | round(1) }}",
        "diagnostic",
        "mdi:memory",
        "ko",
        false,
        nullptr
    };
    const HASensorEntry heapMinFreeBytes{
        "system",
        "sys_hp_min_free",
        "Heap Min Free",
        "rt/system/state",
        "{{ ((value_json.heap.min_free | float(0)) / 1024) | round(1) }}",
        "diagnostic",
        "mdi:memory",
        "ko",
        false,
        nullptr
    };
    const HASensorEntry heapFragPercent{
        "system",
        "sys_hp_frag",
        "Heap Fragmentation",
        "rt/system/state",
        "{{ value_json.heap.frag | int(0) }}",
        "diagnostic",
        "mdi:chart-donut",
        "%",
        false,
        nullptr
    };

    bool ok = true;
    ok = haSvc_->addSensor(haSvc_->ctx, &uptimeMinutes) && ok;
    ok = haSvc_->addSensor(haSvc_->ctx, &heapFreeBytes) && ok;
    ok = haSvc_->addSensor(haSvc_->ctx, &heapMinFreeBytes) && ok;
    ok = haSvc_->addSensor(haSvc_->ctx, &heapFragPercent) && ok;
    if (ok) {
        haEntitiesRegistered_ = true;
    } else {
        LOGD("HA registration failed: system monitor entities");
    }
}

void SystemMonitorModule::onConfigLoaded(ConfigStore&, ServiceRegistry& services)
{
    if (!cfgMqttPub_) {
        cfgMqttPub_ = new (std::nothrow) MqttConfigRouteProducer();
    }
    if (cfgMqttPub_) {
        cfgMqttPub_->configure(this,
                               kSysMonCfgProducerId,
                               kSysMonCfgRoutes,
                               (uint8_t)(sizeof(kSysMonCfgRoutes) / sizeof(kSysMonCfgRoutes[0])),
                               services);
    }
    registerHaEntities_(services);
}

void SystemMonitorModule::logBootInfo() {
    LOGD("Reset reason=%s", SystemStats::resetReasonStr());
    LOGD("CPU=%luMHz", (unsigned long)ESP.getCpuFreqMHz());
    const bool psramOk = psramFound();
    const uint32_t psramSizeBytes = ESP.getPsramSize();
    LOGD("PSRAM found=%s size=%luKB free=%luKB",
         psramOk ? "yes" : "no",
         (unsigned long)(psramSizeBytes / 1024U),
         (unsigned long)(ESP.getFreePsram() / 1024U));
}

void SystemMonitorModule::logHeapStats() {
    SystemStatsSnapshot snap{};
    SystemStats::collect(snap);

    if (!logHub || !logHub->getStats) {
        LOGD("Heap total8 free=%lu min=%lu largest=%lu internal free=%lu min=%lu largest=%lu internal_frag=%u%%",
             (unsigned long)snap.heap.freeBytes,
             (unsigned long)snap.heap.minFreeBytes,
             (unsigned long)snap.heap.largestFreeBlock,
             (unsigned long)snap.heap.internalFreeBytes,
             (unsigned long)snap.heap.internalMinFreeBytes,
             (unsigned long)snap.heap.internalLargestFreeBlock,
             (unsigned int)snap.heap.internalFragPercent);
        return;
    }

    LogHubStatsSnapshot stats{};
    logHub->getStats(logHub->ctx, &stats);
    LOGD("Heap total8 free=%lu min=%lu largest=%lu internal free=%lu min=%lu largest=%lu internal_frag=%u%% LogQ=%u/%u drop=%lu trunc=%lu",
         (unsigned long)snap.heap.freeBytes,
         (unsigned long)snap.heap.minFreeBytes,
         (unsigned long)snap.heap.largestFreeBlock,
         (unsigned long)snap.heap.internalFreeBytes,
         (unsigned long)snap.heap.internalMinFreeBytes,
         (unsigned long)snap.heap.internalLargestFreeBlock,
         (unsigned int)snap.heap.internalFragPercent,
         (unsigned)stats.peakQueued,
         (unsigned)stats.queueLen,
         (unsigned long)stats.droppedCount,
         (unsigned long)stats.formatTruncCount);

    if (stats.droppedCount > 0 || stats.formatTruncCount > 0) {
        const char* lastDropModule =
            (logHub->resolveModuleName && stats.lastDropModuleId != (LogModuleId)LogModuleIdValue::Unknown)
                ? logHub->resolveModuleName(logHub->ctx, stats.lastDropModuleId)
                : "-";
        const char* lastTruncModule =
            (logHub->resolveModuleName && stats.lastFormatTruncModuleId != (LogModuleId)LogModuleIdValue::Unknown)
                ? logHub->resolveModuleName(logHub->ctx, stats.lastFormatTruncModuleId)
                : "-";
        LOGD("LogQ src drop=%s trunc=%s now=%u",
             lastDropModule ? lastDropModule : "-",
             lastTruncModule ? lastTruncModule : "-",
             (unsigned)stats.queuedNow);
    }
}

namespace {
const char* taskStateLabel(TaskHealthState state)
{
    switch (state) {
        case TaskHealthState::Expected: return "expected";
        case TaskHealthState::Starting: return "starting";
        case TaskHealthState::Running: return "running";
        case TaskHealthState::Finishing: return "finishing";
        case TaskHealthState::Finished: return "finished";
        case TaskHealthState::CreateFailed: return "create_failed";
        case TaskHealthState::Disabled: return "disabled";
    }
    return "unknown";
}

const char* taskCadenceLabel(TaskCadence cadence)
{
    switch (cadence) {
        case TaskCadence::Periodic: return "periodic";
        case TaskCadence::EventDriven: return "event";
        case TaskCadence::OneShot: return "one_shot";
    }
    return "unknown";
}

void reportTaskHealth(const ModuleManager::TaskEntry& entry, const TaskHealthSnapshot& snapshot,
                      uint64_t nowUs, bool overdue)
{
    const auto& spec = entry.spec;
    const auto& timing = snapshot.timing;
    const uint64_t progressUs = timing.cycles && timing.lastProgressUs
        ? timing.lastProgressUs : snapshot.startedUs;
    const uint64_t ageMs = snapshot.state == TaskHealthState::Running && nowUs >= progressUs
        ? (nowUs - progressUs) / 1000U : 0U;

    LOGD("TaskSpec id=%u name=%s core=%ld prio=%u stack_b=%lu min_b=%u mode=%s budget_us=%lu slack_us=%lu",
         (unsigned)entry.id, spec.name, (long)spec.coreId, (unsigned)spec.priority,
         (unsigned long)spec.stackSize, (unsigned)spec.minStackFreeBytes, taskCadenceLabel(spec.timing.cadence),
         (unsigned long)spec.timing.workBudgetUs, (unsigned long)spec.timing.wakeToleranceUs);
    char freeBytes[11] = "-";
    if (snapshot.stackSamples) snprintf(freeBytes, sizeof(freeBytes), "%lu", (unsigned long)snapshot.minFreeBytes);
    LOGD("TaskHealth id=%u gen=%lu state=%s samples=%lu free_b=%s cycles=%lu overdue=%u age_ms=%llu",
         (unsigned)entry.id, (unsigned long)snapshot.generation, taskStateLabel(snapshot.state),
         (unsigned long)snapshot.stackSamples, freeBytes, (unsigned long)timing.cycles, (unsigned)overdue,
         (unsigned long long)ageMs);
#if FLOW_PERFORMANCE_DIAGNOSTICS
    LOGD("TaskTime id=%u late=%lu over=%lu max_work_us=%llu max_cycle_us=%llu wait_us=%lu",
         (unsigned)entry.id, (unsigned long)timing.lateStarts, (unsigned long)timing.workOverruns,
         (unsigned long long)timing.maxWorkUs, (unsigned long long)timing.maxCycleUs,
         (unsigned long)timing.expectedWaitUs);
#endif
}

#if FLOW_PERFORMANCE_DIAGNOSTICS
void reportWakeSample(uint16_t taskId, const TaskWakeSample& sample, bool captureOverlap)
{
    LOGD("TaskWake id=%u due_us=%llu run_us=%llu late_us=%llu wait_us=%lu capture=%u",
         (unsigned)taskId, (unsigned long long)sample.expectedUs, (unsigned long long)sample.actualUs,
         (unsigned long long)sample.lateUs(), (unsigned long)sample.waitUs, (unsigned)captureOverlap);
    if (sample.moduleDelayReturnedUs != 0U) {
        const uint64_t waitStartedUs = sample.expectedUs - sample.waitUs;
        LOGD("TaskWakeWait id=%u run_us=%llu module_us=%llu/%lu loop_us=%llu/%lu",
             (unsigned)taskId, (unsigned long long)sample.actualUs,
             (unsigned long long)(sample.moduleDelayReturnedUs - waitStartedUs),
             (unsigned long)sample.moduleWaitUs,
             (unsigned long long)(sample.actualUs - sample.moduleDelayReturnedUs),
             (unsigned long)(sample.waitUs - sample.moduleWaitUs));
    }
    const TaskWaitSample* waits[] = {&sample.moduleWait, &sample.loopWait};
    for (unsigned stage = 0; stage < 2U; ++stage) {
        const auto& wait = *waits[stage];
        if (wait.endedUs == 0U) continue;
        LOGD("TaskWait id=%u stage=%u start_us=%llu end_us=%llu ticks=%lu/%lu",
             (unsigned)taskId, stage, (unsigned long long)wait.startedUs, (unsigned long long)wait.endedUs,
             (unsigned long)wait.elapsedTicks(), (unsigned long)wait.requestedTicks);
        LOGD("TaskWaitCpu id=%u stage=%u window_us=%lu probe_us=%lu accounted_us=%lu matched=%u",
             (unsigned)taskId, stage, (unsigned long)wait.runtimeWindowUs, (unsigned long)wait.probeUs,
             (unsigned long)wait.accountedCpuUs, (unsigned)wait.matchedTasks);
        LOGD("TaskWaitTop id=%u stage=%u cpu_us=%u:%lu,%u:%lu,%u:%lu",
             (unsigned)taskId, stage,
             (unsigned)wait.top[0].taskId, (unsigned long)wait.top[0].durationUs,
             (unsigned)wait.top[1].taskId, (unsigned long)wait.top[1].durationUs,
             (unsigned)wait.top[2].taskId, (unsigned long)wait.top[2].durationUs);
    }
}

void reportTaskWake(const ModuleManager::TaskEntry& entry,
                    const TaskWakeInterval& previousCapture, const TaskWakeInterval& capture)
{
    const auto batch = entry.health.pendingWakeSamples();
    if (batch.count == 0U && batch.dropped == 0U) return;

    unsigned captureCount = 0;
    TaskWakeSample worstCapture{};
    TaskWakeSample worstOther{};
    for (uint8_t i = 0; i < batch.count; ++i) {
        TaskWakeSample sample;
        if (!entry.health.takeWakeSample(sample)) break;
        const bool overlapsCapture = capture.overlaps(sample) || previousCapture.overlaps(sample);
        if (overlapsCapture) ++captureCount;
        auto& worst = overlapsCapture ? worstCapture : worstOther;
        if (sample.lateUs() > worst.lateUs()) worst = sample;
    }

    const bool worstOverlaps = capture.overlaps(batch.worst) || previousCapture.overlaps(batch.worst);
    auto& worst = worstOverlaps ? worstCapture : worstOther;
    if (batch.worst.lateUs() > worst.lateUs()) worst = batch.worst;
    LOGD("TaskWakeStats id=%u samples=%u capture=%u dropped=%lu max_us=%llu",
         (unsigned)entry.id, (unsigned)batch.count, captureCount, (unsigned long)batch.dropped,
         (unsigned long long)batch.worst.lateUs());
    if (worstCapture.actualUs != 0U) reportWakeSample(entry.id, worstCapture, true);
    if (worstOther.actualUs != 0U) reportWakeSample(entry.id, worstOther, false);
}
#endif
}  // namespace

void SystemMonitorModule::logTaskHealth(SystemHealthSnapshot::Tasks& summary)
{
    if (!moduleManager) return;
    const size_t taskCount = moduleManager->getTaskEntryCount();
    if (taskCount == 0U) return;
    const uint64_t nowUs = esp_timer_get_time();
    for (uint8_t i = 0; i < taskCount; ++i) {
        const auto& entry = moduleManager->getTaskEntry(i);
        auto snapshot = entry.health.snapshot();
        const auto stack = readTaskStack_(entry.id);
        const bool low = snapshot.stackSamples && snapshot.minFreeBytes < entry.spec.minStackFreeBytes;
        const bool overdue = snapshot.state == TaskHealthState::Running &&
                             snapshot.timing.overdue(nowUs, entry.spec.timing);
        if (snapshot.state == TaskHealthState::Running) ++summary.running;
        if (low) ++summary.belowStackThreshold;
        summary.lateStarts += snapshot.timing.lateStarts;
        summary.workOverruns += snapshot.timing.workOverruns;
        if (snapshot.stackSamples) {
            auto& lowest = summary.lowestMargin;
            const auto margin = int64_t(snapshot.minFreeBytes) - entry.spec.minStackFreeBytes;
            const auto lowestMargin = int64_t(lowest.freeBytes) - lowest.thresholdBytes;
            if (!lowest.task || margin < lowestMargin) {
                lowest = {entry.spec.name, snapshot.minFreeBytes, entry.spec.minStackFreeBytes, stack.sampledAtUs};
            }
        }
        auto& longest = summary.longestWork;
        if (snapshot.timing.cycles && (!longest.task || snapshot.timing.maxWorkUs > longest.durationUs)) {
            longest = {entry.spec.name, snapshot.timing.maxWorkUs};
        }
        auto& previous = taskAlerts_[i];
        const bool critical = entry.spec.timing.workBudgetUs != 0U;
        if ((low && !previous.lowStack) ||
            (critical && ((overdue && !previous.overdue) ||
                          snapshot.timing.lateStarts > previous.lateStarts ||
                          snapshot.timing.workOverruns > previous.workOverruns))) {
            LOGD("Task alert id=%u name=%s low_stack=%u overdue=%u late=%lu over=%lu",
                 (unsigned)entry.id, entry.spec.name, (unsigned)low, (unsigned)overdue,
                 (unsigned long)snapshot.timing.lateStarts, (unsigned long)snapshot.timing.workOverruns);
        }
        previous = {snapshot.timing.lateStarts, snapshot.timing.workOverruns, low, overdue};

        if ((i + taskCount - nextTaskHealthReport_) % taskCount < kTaskHealthReportBatch) {
            reportTaskHealth(entry, snapshot, nowUs, overdue);
            if (stack.samples) {
                LOGD("TaskStack id=%u gen=%lu free_b=%lu age_ms=%llu last_us=%llu max_us=%llu",
                     (unsigned)entry.id, (unsigned long)stack.generation, (unsigned long)stack.freeBytes,
                     (unsigned long long)((nowUs - stack.sampledAtUs) / 1000U),
                     (unsigned long long)stack.lastDurationUs, (unsigned long long)stack.maxDurationUs);
            }
        }
#if FLOW_PERFORMANCE_DIAGNOSTICS
        if (entry.spec.traceWake) reportTaskWake(entry, {}, lastTaskCapture_);
#endif
    }
    nextTaskHealthReport_ = (nextTaskHealthReport_ + kTaskHealthReportBatch) % taskCount;
    if (!stackSlots_) return;
    for (uint8_t i = 0; i < externalStackCount_; ++i) {
        const auto& slot = stackSlots_[Limits::Core::Capacity::MaxTaskHealthEntries + i];
        const auto sample = readTaskStack_(slot.id);
        if (!sample.samples) continue;
        const auto& source = slot.source;
        if (sample.freeBytes < source.minFreeBytes) ++summary.belowStackThreshold;
        auto& lowest = summary.lowestMargin;
        if (!lowest.task || int64_t(sample.freeBytes) - source.minFreeBytes <
                            int64_t(lowest.freeBytes) - lowest.thresholdBytes) {
            lowest = {source.name, sample.freeBytes, source.minFreeBytes, sample.sampledAtUs};
        }
        LOGD("TaskStack id=%u name=%s gen=%lu free_b=%lu min_b=%u age_ms=%llu max_us=%llu",
             (unsigned)slot.id, source.name, (unsigned long)sample.generation, (unsigned long)sample.freeBytes,
             (unsigned)source.minFreeBytes, (unsigned long long)((nowUs - sample.sampledAtUs) / 1000U),
             (unsigned long long)sample.maxDurationUs);
    }
}

TaskStackSample SystemMonitorModule::readTaskStack_(uint16_t taskId)
{
    TaskStackSample result{};
    if (!stackSlots_) return result;
    portENTER_CRITICAL(&stackSamplesMux_);
    const size_t capacity = Limits::Core::Capacity::MaxTaskHealthEntries + externalStackCount_;
    for (size_t i = 0; i < capacity; ++i) {
        if (stackSlots_[i].id == taskId) {
            result = stackSlots_[i].sample;
            break;
        }
    }
    portEXIT_CRITICAL(&stackSamplesMux_);
    return result;
}

void SystemMonitorModule::readCore1Runtime_(TaskRuntimeSnapshot& out)
{
#if FLOW_PERFORMANCE_DIAGNOSTICS
    out.count = 0U;
    out.available = false;
    out.startedUs = esp_timer_get_time();
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    if (!moduleManager) return;
    const auto caller = xTaskGetCurrentTaskHandle();
    auto append = [&out](TaskHandle_t handle, uint16_t id, uint32_t generation) {
        if (out.count == TaskRuntimeSnapshot::kCapacity) return;
        TaskStatus_t status{};
        // eReady skips the state query and pdFALSE skips the stack scan.
        vTaskGetInfo(handle, &status, pdFALSE, eReady);
        if (status.xCoreID == 1) {
            out.tasks[out.count++] = {static_cast<uint32_t>(status.ulRunTimeCounter), generation, id};
        }
    };
    for (uint8_t i = 0; i < moduleManager->getTaskEntryCount(); ++i) {
        const auto& entry = moduleManager->getTaskEntry(i);
        if (entry.spec.coreId != 1) continue;
        TaskHealthSnapshot snapshot{};
        if (!entry.health.acquireStackRead(snapshot)) continue;
        if (snapshot.handle != caller) append(snapshot.handle, entry.id, snapshot.generation);
        entry.health.releaseStackRead();
    }
    if (stackSlots_) {
        for (uint8_t i = 0; i < externalStackCount_; ++i) {
            const auto& slot = stackSlots_[Limits::Core::Capacity::MaxTaskHealthEntries + i];
            if (slot.source.coreId != 1 || !readTaskStack_(slot.id).samples) continue;
            TaskHandle_t handle = nullptr;
            uint32_t generation = 0;
            if (slot.source.acquire(slot.source.context, handle, generation) != TaskStackReadStatus::Acquired) continue;
            append(handle, slot.id, generation);
            if (slot.source.release) slot.source.release(slot.source.context);
        }
    }
    append(xTaskGetIdleTaskHandleForCore(1), taskHealthId(ModuleId::Unknown, TaskRole::IdleCore1), 1U);
    out.available = out.count < TaskRuntimeSnapshot::kCapacity;
#endif
    out.endedUs = esp_timer_get_time();
#else
    out = {};
#endif
}

void SystemMonitorModule::sampleTaskStack_(uint32_t nowMs)
{
    if (!stackSlots_ || !moduleManager || nowMs - lastStackSampleMs_ < kStackSamplePeriodMs) return;
    lastStackSampleMs_ = nowMs;
    const uint8_t taskCount = moduleManager->getTaskEntryCount();
    const uint8_t count = taskCount + externalStackCount_;
    if (count == 0U) return;
    const uint8_t index = nextStackSample_ % count;
    nextStackSample_ = (index + 1U) % count;
    const bool external = index >= taskCount;
    auto& slot = stackSlots_[external ? Limits::Core::Capacity::MaxTaskHealthEntries + index - taskCount : index];
    TaskHandle_t handle = nullptr;
    uint32_t generation = 0;
    TaskHealth* health = nullptr;
    if (external) {
        const auto status = slot.source.acquire(slot.source.context, handle, generation);
        if (status != TaskStackReadStatus::Acquired) {
            if (status == TaskStackReadStatus::Unavailable) {
                portENTER_CRITICAL(&stackSamplesMux_);
                slot.sample = {};
                portEXIT_CRITICAL(&stackSamplesMux_);
            }
            return;
        }
    } else {
        const auto& entry = moduleManager->getTaskEntry(index);
        health = &entry.health;
        TaskHealthSnapshot snapshot{};
        if (!health->acquireStackRead(snapshot)) return;
        handle = snapshot.handle;
        generation = snapshot.generation;
        portENTER_CRITICAL(&stackSamplesMux_);
        slot.id = entry.id;
        portEXIT_CRITICAL(&stackSamplesMux_);
    }

#if FLOW_PERFORMANCE_DIAGNOSTICS
    const uint64_t startedUs = esp_timer_get_time();
#endif
    const uint32_t freeBytes = uxTaskGetStackHighWaterMark(handle);
    const uint64_t endedUs = esp_timer_get_time();
    if (health) {
        health->recordStack(handle, generation, freeBytes);
        health->releaseStackRead();
    } else if (slot.source.release) {
        slot.source.release(slot.source.context);
    }
    portENTER_CRITICAL(&stackSamplesMux_);
    auto& sample = slot.sample;
    if (sample.generation != generation) sample = {};
    sample.generation = generation;
    sample.sampledAtUs = endedUs;
    sample.freeBytes = std::min(sample.freeBytes, freeBytes);
#if FLOW_PERFORMANCE_DIAGNOSTICS
    sample.lastDurationUs = endedUs - startedUs;
    sample.maxDurationUs = std::max(sample.maxDurationUs, sample.lastDurationUs);
#endif
    incrementSaturated(sample.samples);
    portEXIT_CRITICAL(&stackSamplesMux_);
}

bool SystemMonitorModule::setDiagnosticsMode_(void* ctx, const CommandRequest& req,
                                              char* reply, size_t replyLen)
{
    if (!ctx || !reply || !replyLen) return false;
    auto& self = *static_cast<SystemMonitorModule*>(ctx);
    const char* requested = req.args ? req.args : "";
    if (strcmp(requested, "normal") == 0) {
        self.diagnosticsMode_.store(0U);
        snprintf(reply, replyLen, "{\"ok\":true,\"mode\":\"normal\"}");
        return true;
    }
    if (strcmp(requested, "detailed") == 0) {
        self.diagnosticsMode_.store(1U);
        snprintf(reply, replyLen, "{\"ok\":true,\"mode\":\"detailed\"}");
        return true;
    }
    snprintf(reply, replyLen,
             "{\"ok\":false,\"error\":\"expected_args_normal_or_detailed\"}");
    return false;
}

bool SystemMonitorModule::requestTaskCapture_(void* ctx, const CommandRequest&, char* reply, size_t replyLen)
{
    if (!ctx || !reply || !replyLen) return false;
#if configUSE_TRACE_FACILITY
    auto& self = *static_cast<SystemMonitorModule*>(ctx);
    // One snapshot, on the low-priority sysmon task, not IO or AsyncTCP.
    // Limit operator-triggered captures so they cannot saturate the scheduler.
    constexpr uint32_t kCaptureCooldownMs = 10000U;
    const uint32_t nowMs = millis();
    uint32_t previous = self.lastTaskCaptureRequestMs_.load();
    do {
        if (previous != 0U && nowMs - previous < kCaptureCooldownMs) {
            snprintf(reply, replyLen, "{\"ok\":false,\"error\":\"capture_cooldown\"}");
            return false;
        }
    } while (!self.lastTaskCaptureRequestMs_.compare_exchange_weak(previous, nowMs));
    self.captureRequested_.store(true);
    const int written = snprintf(reply, replyLen,
                                 "{\"ok\":true,\"queued\":true,\"mode\":\"capture_once\"}");
    return written > 0 && size_t(written) < replyLen;
#else
    snprintf(reply, replyLen, "{\"ok\":false,\"error\":\"trace_unavailable\"}");
    return false;
#endif
}

void SystemMonitorModule::captureTasksIfRequested_()
{
#if configUSE_TRACE_FACILITY
    if (!captureRequested_.exchange(false)) return;
    auto* tasks = static_cast<TaskStatus_t*>(heap_caps_calloc(
        kTaskStatusSnapshotCapacity, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tasks) {
        LOGD("Task capture unavailable: PSRAM allocation failed");
        return;
    }
#if FLOW_PERFORMANCE_DIAGNOSTICS
    const uint64_t startedUs = esp_timer_get_time();
#endif
    const UBaseType_t captured = uxTaskGetSystemState(tasks, kTaskStatusSnapshotCapacity, nullptr);
#if FLOW_PERFORMANCE_DIAGNOSTICS
    const uint64_t endedUs = esp_timer_get_time();
    lastTaskCapture_ = {startedUs, endedUs};
    taskCaptureDurations_.record(endedUs - startedUs);
    LOGD("TaskCapture start_us=%llu last_us=%llu max_us=%llu samples=%lu tasks=%u",
         (unsigned long long)startedUs, (unsigned long long)taskCaptureDurations_.lastUs,
         (unsigned long long)taskCaptureDurations_.maxUs, (unsigned long)taskCaptureDurations_.count,
         (unsigned)captured);
#endif
    if (captured == 0U) LOGD("Task capture unavailable capacity=%u", (unsigned)kTaskStatusSnapshotCapacity);
    for (UBaseType_t i = 0; i < captured; ++i) {
        const auto& task = tasks[i];
        if (task.eCurrentState == eDeleted) continue;
#if configTASKLIST_INCLUDE_COREID
        const long core = task.xCoreID == tskNO_AFFINITY ? -1L : task.xCoreID;
#else
        const long core = -1;
#endif
        // The SDK task name points into a TCB that may already have been freed.
#if configGENERATE_RUN_TIME_STATS
        // Cumulative runtime counter for a manually requested one-shot capture.
        // This is not a per-cycle sample and adds no work to the IO task.
        LOGD("SdkTask id=%u core=%ld prio=%u free_b=%lu runtime_counter=%lu",
             (unsigned)task.xTaskNumber, core, (unsigned)task.uxBasePriority,
             (unsigned long)task.usStackHighWaterMark, (unsigned long)task.ulRunTimeCounter);
#else
        LOGD("SdkTask id=%u core=%ld prio=%u free_b=%lu", (unsigned)task.xTaskNumber, core,
             (unsigned)task.uxBasePriority, (unsigned long)task.usStackHighWaterMark);
#endif
    }
    heap_caps_free(tasks);
#endif
}

void SystemMonitorModule::logEventBusHealth(const EventBus::QueueSnapshot& queue)
{
    LOGD("EventQueue id=%u queued=%u cap=%u peak=%u accepted=%lu dequeued=%lu retries=%lu",
         (unsigned)queue.id, (unsigned)queue.queued, (unsigned)queue.capacity, (unsigned)queue.peak,
         (unsigned long)queue.accepted, (unsigned long)queue.dequeued, (unsigned long)queue.fullRetries);
    LOGD("EventDelay id=%u head=%u head_src=%u age_us=%llu slowest=%u slow_src=%u max_us=%llu",
         (unsigned)queue.id, (unsigned)queue.oldestEvent, (unsigned)queue.oldestProducer,
         (unsigned long long)queue.oldestAgeUs, (unsigned)queue.slowestEvent,
         (unsigned)queue.slowestProducer, (unsigned long long)queue.maxQueueDelayUs);

    LOGD("EventDrop id=%u full=%lu oversize=%lu no_payload=%lu isr=%lu control=%lu sub=%lu last=%u src=%u",
         (unsigned)queue.id, (unsigned long)queue.fullDrops, (unsigned long)queue.oversizeDrops,
         (unsigned long)queue.missingPayloadDrops, (unsigned long)queue.isrDrops,
         (unsigned long)queue.controlDrops, (unsigned long)queue.subscriptionDrops,
         (unsigned)queue.lastDroppedEvent, (unsigned)queue.lastDroppedProducer);
}

SystemHealthSnapshot SystemMonitorModule::readHealthSnapshot_()
{
    portENTER_CRITICAL(&healthSnapshotMux_);
    const auto summary = healthSnapshot_;
    portEXIT_CRITICAL(&healthSnapshotMux_);
    return summary;
}

void SystemMonitorModule::sampleSystemHealth_()
{
    SystemHealthSnapshot summary{};
    logTaskHealth(summary.tasks);
    if (eventBus_) {
        summary.eventBusAvailable = true;
        summary.eventBus = eventBus_->queueSnapshot();
        logEventBusHealth(summary.eventBus);
    }
    summary.sampledAtUs = esp_timer_get_time();
    portENTER_CRITICAL(&healthSnapshotMux_);
    healthSnapshot_ = summary;
    portEXIT_CRITICAL(&healthSnapshotMux_);
}

void SystemMonitorModule::logTrackedBuffers()
{
    TrackedBufferSnapshot snapshots[(size_t)TrackedBufferId::Count]{};
    const size_t count = BufferUsageTracker::snapshot(snapshots, (size_t)TrackedBufferId::Count);
    static constexpr char kBufPrefix[] = "Buf ";
    static constexpr size_t kLineMsgBudget = (size_t)LOG_MSG_MAX - sizeof(kBufPrefix);
    char line[kLineMsgBudget + 1];
    size_t off = 0;
    uint8_t itemsOnLine = 0;
    bool loggedAny = false;
    line[0] = '\0';

    for (size_t i = 0; i < count; ++i) {
        const TrackedBufferSnapshot& s = snapshots[i];
        if (!s.name) continue;
        if (s.peakUsed == 0U && s.capacity == 0U) continue;

        char entry[96];
        const bool isFull = (s.capacity > 0U) && (s.peakUsed >= s.capacity);
        const int ew = s.source[0]
            ? snprintf(entry, sizeof(entry), "%s=%lu/%lu%s@%s",
                       s.name,
                       (unsigned long)s.peakUsed,
                       (unsigned long)s.capacity,
                       isFull ? "!" : "",
                       s.source)
            : snprintf(entry, sizeof(entry), "%s=%lu/%lu%s",
                       s.name,
                       (unsigned long)s.peakUsed,
                       (unsigned long)s.capacity,
                       isFull ? "!" : "");
        if (ew <= 0) continue;

        const size_t entryLen = (size_t)ew;
        const size_t sepLen = (itemsOnLine > 0U) ? 1U : 0U;
        if (itemsOnLine > 0U && (off + sepLen + entryLen) > kLineMsgBudget) {
            LOGD("Buf %s", line);
            line[0] = '\0';
            off = 0;
            itemsOnLine = 0;
        }
        if ((off + ((itemsOnLine > 0U) ? 1U : 0U) + entryLen) > kLineMsgBudget) {
            if (entryLen > kLineMsgBudget) continue;
        }
        if (itemsOnLine > 0U) {
            line[off++] = ' ';
            line[off] = '\0';
        }
        memcpy(line + off, entry, entryLen + 1U);
        off += entryLen;
        ++itemsOnLine;
        loggedAny = true;
    }

    if (!loggedAny) {
        LOGD("Buf none");
        return;
    }

    if (itemsOnLine > 0U) {
        LOGD("Buf %s", line);
    }
}

void SystemMonitorModule::appendHeapWatchSample_(const SystemStatsSnapshot& snap)
{
    if (!heapWatchSamples_) return;
    HeapWatchSample& sample = heapWatchSamples_[heapWatchWriteIndex_];
    sample.uptimeMs = snap.uptimeMs;
    sample.freeBytes = snap.heap.freeBytes;
    sample.minFreeBytes = snap.heap.minFreeBytes;
    sample.largestFreeBlock = snap.heap.largestFreeBlock;
    sample.internalFreeBytes = snap.heap.internalFreeBytes;
    sample.internalMinFreeBytes = snap.heap.internalMinFreeBytes;
    sample.internalLargestFreeBlock = snap.heap.internalLargestFreeBlock;

    heapWatchWriteIndex_ = (heapWatchWriteIndex_ + 1U) % kHeapWatchSampleCount;
    if (heapWatchCount_ < kHeapWatchSampleCount) {
        ++heapWatchCount_;
    }
}

void SystemMonitorModule::armHeapWatchDump_(const SystemStatsSnapshot& snap, const char* reason)
{
    heapWatchTripActive_ = true;
    heapWatchDumpPending_ = true;
    heapWatchTriggerMs_ = snap.uptimeMs;
    heapWatchTriggerFreeBytes_ = snap.heap.internalFreeBytes;
    heapWatchTriggerMinFreeBytes_ = snap.heap.internalMinFreeBytes;
    heapWatchTriggerLargestFreeBlock_ = snap.heap.internalLargestFreeBlock;
    heapWatchFrozenWriteIndex_ = heapWatchWriteIndex_;
    heapWatchFrozenCount_ = heapWatchCount_;
    snprintf(heapWatchTriggerReason_, sizeof(heapWatchTriggerReason_), "%s", reason ? reason : "-");
#ifdef CONFIG_HEAP_TASK_TRACKING
    captureHeapWatchTaskTotals_();
#endif
    LOGD("HeapWatch trip captured reason=%s free=%lu min=%lu largest=%lu",
         heapWatchTriggerReason_,
         (unsigned long)heapWatchTriggerFreeBytes_,
         (unsigned long)heapWatchTriggerMinFreeBytes_,
         (unsigned long)heapWatchTriggerLargestFreeBlock_);
}

void SystemMonitorModule::dumpHeapWatchWindow_() const
{
    if (!heapWatchSamples_ || heapWatchFrozenCount_ == 0U) {
        LOGD("HeapWatch window unavailable");
        return;
    }

    const size_t sampleCount =
        (heapWatchFrozenCount_ < kHeapWatchDumpSampleCount) ? heapWatchFrozenCount_ : kHeapWatchDumpSampleCount;
    const size_t startIndex =
        (heapWatchFrozenWriteIndex_ + kHeapWatchSampleCount - sampleCount) % kHeapWatchSampleCount;

    for (size_t i = 0; i < sampleCount; ++i) {
        const size_t idx = (startIndex + i) % kHeapWatchSampleCount;
        const HeapWatchSample& sample = heapWatchSamples_[idx];
        const long dtMs = (long)sample.uptimeMs - (long)heapWatchTriggerMs_;
        LOGD("HeapWatch win dt=%ld total_free=%lu total_min=%lu total_largest=%lu internal_free=%lu internal_min=%lu internal_largest=%lu",
             dtMs,
             (unsigned long)sample.freeBytes,
             (unsigned long)sample.minFreeBytes,
             (unsigned long)sample.largestFreeBlock,
             (unsigned long)sample.internalFreeBytes,
             (unsigned long)sample.internalMinFreeBytes,
             (unsigned long)sample.internalLargestFreeBlock);
    }
}

#ifdef CONFIG_HEAP_TASK_TRACKING
void SystemMonitorModule::captureHeapWatchTaskTotals_()
{
    heapWatchTaskTotalCount_ = 0U;

    heap_task_totals_t totals[kHeapWatchTaskTotalsMax]{};
    size_t numTotals = 0U;
    heap_task_info_params_t params{};
    params.caps[0] = 0;
    params.mask[0] = 0;
    params.totals = totals;
    params.num_totals = &numTotals;
    params.max_totals = kHeapWatchTaskTotalsMax;

    heap_caps_get_per_task_info(&params);

    const size_t totalCount = (numTotals < kHeapWatchTaskTotalsMax) ? numTotals : kHeapWatchTaskTotalsMax;
    for (size_t i = 0; i < totalCount; ++i) {
        heapWatchTaskTotals_[i].sizeBytes = (uint32_t)totals[i].size[0];
        heapWatchTaskTotals_[i].blockCount = (uint32_t)totals[i].count[0];
        copyTaskName_(heapWatchTaskTotals_[i].taskName, sizeof(heapWatchTaskTotals_[i].taskName), totals[i].task);
    }
    heapWatchTaskTotalCount_ = totalCount;
    for (size_t i = 0; i < heapWatchTaskTotalCount_; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < heapWatchTaskTotalCount_; ++j) {
            if (heapWatchTaskTotals_[j].sizeBytes > heapWatchTaskTotals_[best].sizeBytes) {
                best = j;
            }
        }
        if (best == i) continue;
        HeapWatchTaskTotal tmp = heapWatchTaskTotals_[i];
        heapWatchTaskTotals_[i] = heapWatchTaskTotals_[best];
        heapWatchTaskTotals_[best] = tmp;
    }
}

void SystemMonitorModule::dumpHeapWatchTaskTotals_() const
{
    if (heapWatchTaskTotalCount_ == 0U) {
        LOGD("HeapWatch tasks unavailable");
        return;
    }

    for (size_t i = 0; i < heapWatchTaskTotalCount_; ++i) {
        const HeapWatchTaskTotal& total = heapWatchTaskTotals_[i];
        if (total.sizeBytes == 0U && total.blockCount == 0U) continue;
        LOGD("HeapWatch task=%s alloc=%lu blocks=%lu",
             total.taskName[0] ? total.taskName : "-",
             (unsigned long)total.sizeBytes,
             (unsigned long)total.blockCount);
    }
}
#endif

void SystemMonitorModule::dumpHeapWatch_()
{
    LOGD("HeapWatch dump reason=%s trigger_free=%lu trigger_min=%lu trigger_largest=%lu captured=%u dump_last=%u sample_ms=%lu",
         heapWatchTriggerReason_[0] ? heapWatchTriggerReason_ : "-",
         (unsigned long)heapWatchTriggerFreeBytes_,
         (unsigned long)heapWatchTriggerMinFreeBytes_,
         (unsigned long)heapWatchTriggerLargestFreeBlock_,
         (unsigned)heapWatchFrozenCount_,
         (unsigned)kHeapWatchDumpSampleCount,
         (unsigned long)kHeapWatchSamplePeriodMs);
    dumpHeapWatchWindow_();
#ifdef CONFIG_HEAP_TASK_TRACKING
    dumpHeapWatchTaskTotals_();
#endif
    heapWatchDumpPending_ = false;
}

void SystemMonitorModule::pollHeapWatch_(uint32_t now)
{
    if (lastHeapWatchSampleMs_ != 0U && (uint32_t)(now - lastHeapWatchSampleMs_) < kHeapWatchSamplePeriodMs) {
        return;
    }
    lastHeapWatchSampleMs_ = now;

    SystemStatsSnapshot snap{};
    SystemStats::collect(snap);
    appendHeapWatchSample_(snap);

    if (heapWatchLastSeenMinFree_ == UINT32_MAX) {
        heapWatchLastSeenMinFree_ = snap.heap.internalMinFreeBytes;
    }

    const bool currentFreeTrip = snap.heap.internalFreeBytes <= kHeapWatchTripFreeBytes;
    const bool minLowWaterTrip =
        snap.heap.internalMinFreeBytes <= kHeapWatchTripFreeBytes &&
        snap.heap.internalMinFreeBytes < heapWatchLastSeenMinFree_;

    if (!heapWatchTripActive_ && !heapWatchDumpPending_) {
        if (currentFreeTrip) {
            armHeapWatchDump_(snap, "free");
        } else if (minLowWaterTrip) {
            armHeapWatchDump_(snap, "min_low");
        }
    }

    if (snap.heap.internalMinFreeBytes < heapWatchLastSeenMinFree_) {
        heapWatchLastSeenMinFree_ = snap.heap.internalMinFreeBytes;
    }

    if (heapWatchDumpPending_) {
        const bool recovered = snap.heap.internalFreeBytes >= kHeapWatchRecoverFreeBytes;
        const bool timeout = (uint32_t)(snap.uptimeMs - heapWatchTriggerMs_) >= kHeapWatchDumpDelayMs;
        if (recovered || timeout) {
            dumpHeapWatch_();
        }
    }

    if (heapWatchTripActive_ && snap.heap.internalFreeBytes >= kHeapWatchRecoverFreeBytes) {
        LOGD("HeapWatch recovered internal_free=%lu internal_min=%lu internal_largest=%lu",
             (unsigned long)snap.heap.internalFreeBytes,
             (unsigned long)snap.heap.internalMinFreeBytes,
             (unsigned long)snap.heap.internalLargestFreeBlock);
        heapWatchTripActive_ = false;
    }
}

void SystemMonitorModule::logPendingHeapAllocFailure_()
{
    if (!gHeapAllocFailedPending) return;

    const size_t size = gHeapAllocFailedSize;
    const uint32_t caps = gHeapAllocFailedCaps;
    const char* functionName = gHeapAllocFailedFunction;
    gHeapAllocFailedPending = false;

    SystemStatsSnapshot snap{};
    SystemStats::collect(snap);
    LOGD("Heap alloc failed size=%lu caps=0x%08lx func=%s total_free=%lu total_min=%lu total_largest=%lu internal_free=%lu internal_min=%lu internal_largest=%lu",
         (unsigned long)size,
         (unsigned long)caps,
         functionName ? functionName : "-",
         (unsigned long)snap.heap.freeBytes,
         (unsigned long)snap.heap.minFreeBytes,
         (unsigned long)snap.heap.largestFreeBlock,
         (unsigned long)snap.heap.internalFreeBytes,
         (unsigned long)snap.heap.internalMinFreeBytes,
         (unsigned long)snap.heap.internalLargestFreeBlock);
}

void SystemMonitorModule::pollMemoryPressureReboot_(uint32_t now)
{
    const uint32_t bootGraceMs =
        (cfgData_.webWatchdogBootGraceMs > (int32_t)kWebWatchdogMinBootGraceMs)
            ? (uint32_t)cfgData_.webWatchdogBootGraceMs
            : kWebWatchdogMinBootGraceMs;
    if (now < bootGraceMs) {
        memoryPressureState_ = (uint8_t)MemoryPressureState::Normal;
        memoryPressureStateSinceMs_ = now;
        memoryPressureRebootIssued_ = false;
        return;
    }

    if (!fwUpdateSvc_ && services_) {
        fwUpdateSvc_ = services_->get<FirmwareUpdateService>(ServiceId::FirmwareUpdate);
    }
    const bool firmwareUpdateBusy =
        fwUpdateSvc_ && fwUpdateSvc_->isBusy && fwUpdateSvc_->isBusy(fwUpdateSvc_->ctx);

    SystemStatsSnapshot snap{};
    SystemStats::collect(snap);
    const MemoryPressureState state = applyMemoryPressureHysteresis_(
        snap, (MemoryPressureState)memoryPressureState_);
    const uint8_t stateRaw = (uint8_t)state;

    if (stateRaw != memoryPressureState_) {
        const MemoryPressureState prevState = (MemoryPressureState)memoryPressureState_;
        const bool prevCriticalOrWorse =
            (prevState == MemoryPressureState::Critical) || (prevState == MemoryPressureState::Panic);
        const bool nowCriticalOrWorse =
            (state == MemoryPressureState::Critical) || (state == MemoryPressureState::Panic);
        const bool suppressTransitionLog = firmwareUpdateBusy && !prevCriticalOrWorse && !nowCriticalOrWorse;

        memoryPressureState_ = stateRaw;
        memoryPressureStateSinceMs_ = now;
        memoryPressureRebootIssued_ = false;
        if (!suppressTransitionLog) {
            LOGD("Memory pressure -> %s internal_free=%lu internal_largest=%lu internal_frag=%u%%%s",
                 memoryPressureStateStr_(state),
                 (unsigned long)snap.heap.internalFreeBytes,
                 (unsigned long)snap.heap.internalLargestFreeBlock,
                 (unsigned int)snap.heap.internalFragPercent,
                 firmwareUpdateBusy ? " fwupdate=busy" : "");
        }
        return;
    }

    if (memoryPressureRebootIssued_) return;
    if (!cfgData_.webWatchdogAutoReboot) return;

    const uint32_t sinceMs = memoryPressureStateSinceMs_;
    const uint32_t heldMs = (sinceMs > 0U) ? (uint32_t)(now - sinceMs) : 0U;

    bool shouldReboot = false;
    if (state == MemoryPressureState::Panic && heldMs >= kPressurePanicRebootDelayMs) {
        shouldReboot = true;
    } else if (state == MemoryPressureState::Critical && heldMs >= kPressureCriticalRebootDelayMs) {
        shouldReboot = true;
    }
    if (!shouldReboot) return;

    memoryPressureRebootIssued_ = true;
    LOGD("Memory pressure persisted (%s %lums), reboot requested internal_free=%lu internal_largest=%lu internal_frag=%u%%",
         memoryPressureStateStr_(state),
         (unsigned long)heldMs,
         (unsigned long)snap.heap.internalFreeBytes,
         (unsigned long)snap.heap.internalLargestFreeBlock,
         (unsigned int)snap.heap.internalFragPercent);
    if (!cmdSvc_ && services_) {
        cmdSvc_ = services_->get<CommandService>(ServiceId::Command);
    }
    if (cmdSvc_ && cmdSvc_->execute) {
        char reply[160] = {0};
        (void)cmdSvc_->execute(cmdSvc_->ctx, "system.reboot", "{}", nullptr, systemActor(), reply, sizeof(reply));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void SystemMonitorModule::pollWebWatchdog_(uint32_t now)
{
    if (!cfgData_.webWatchdogEnabled) {
        webWatchdogConsecutiveFailures_ = 0U;
        webWatchdogRebootIssued_ = false;
        return;
    }

    const uint32_t checkPeriodMs =
        (cfgData_.webWatchdogCheckPeriodMs > (int32_t)kWebWatchdogMinCheckPeriodMs)
            ? (uint32_t)cfgData_.webWatchdogCheckPeriodMs
            : kWebWatchdogMinCheckPeriodMs;
    if (lastWebWatchdogCheckMs_ != 0U && (uint32_t)(now - lastWebWatchdogCheckMs_) < checkPeriodMs) {
        return;
    }
    lastWebWatchdogCheckMs_ = now;

    const uint32_t bootGraceMs =
        (cfgData_.webWatchdogBootGraceMs > (int32_t)kWebWatchdogMinBootGraceMs)
            ? (uint32_t)cfgData_.webWatchdogBootGraceMs
            : kWebWatchdogMinBootGraceMs;
    if (now < bootGraceMs) {
        webWatchdogConsecutiveFailures_ = 0U;
        webWatchdogRebootIssued_ = false;
        return;
    }

    if (!services_) return;
    if (!netAccessSvc_) {
        netAccessSvc_ = services_->get<NetworkAccessService>(ServiceId::NetworkAccess);
    }
    if (!webInterfaceSvc_) {
        webInterfaceSvc_ = services_->get<WebInterfaceService>(ServiceId::WebInterface);
    }
    if (!cmdSvc_) {
        cmdSvc_ = services_->get<CommandService>(ServiceId::Command);
    }
    if (!webInterfaceSvc_ || !webInterfaceSvc_->getHealth) return;

    NetworkAccessMode mode = NetworkAccessMode::None;
    bool webReachable = false;
    if (netAccessSvc_) {
        if (netAccessSvc_->mode) mode = netAccessSvc_->mode(netAccessSvc_->ctx);
        if (netAccessSvc_->isWebReachable) {
            webReachable = netAccessSvc_->isWebReachable(netAccessSvc_->ctx);
        } else {
            webReachable = (mode != NetworkAccessMode::None);
        }
    }
    if (!webReachable || mode == NetworkAccessMode::None) {
        webWatchdogConsecutiveFailures_ = 0U;
        webWatchdogRebootIssued_ = false;
        return;
    }

    WebInterfaceHealth health{};
    if (!webInterfaceSvc_->getHealth(webInterfaceSvc_->ctx, &health)) return;

    // Sample after the snapshot: the web task may have advanced since poll entry.
    // Unsigned subtraction then also handles a millis() wrap between samples.
    const uint32_t healthNowMs = millis();

    if (!health.started || health.paused) {
        webWatchdogConsecutiveFailures_ = 0U;
        webWatchdogRebootIssued_ = false;
        return;
    }

    const uint32_t staleMs =
        (cfgData_.webWatchdogStaleMs > (int32_t)kWebWatchdogMinStaleMs)
            ? (uint32_t)cfgData_.webWatchdogStaleMs
            : kWebWatchdogMinStaleMs;
    const uint32_t loopAgeMs = (health.lastLoopMs > 0U) ? (uint32_t)(healthNowMs - health.lastLoopMs) : UINT32_MAX;
    const bool loopStale = (health.lastLoopMs == 0U) || (loopAgeMs > staleMs);

    const uint16_t activeClients = (uint16_t)(health.wsSerialClients + health.wsLogClients);
    const uint32_t wsIdleMs = health.lastWsActivityMs > 0U
        ? (uint32_t)(healthNowMs - health.lastWsActivityMs) : UINT32_MAX;
    const uint32_t httpIdleMs = health.lastHttpActivityMs > 0U
        ? (uint32_t)(healthNowMs - health.lastHttpActivityMs) : UINT32_MAX;
    // Compare elapsed ages, not raw timestamps, across millis() rollover.
    const uint32_t clientIdleMs = activeClients == 0U ? 0U
        : (wsIdleMs < httpIdleMs ? wsIdleMs : httpIdleMs);
    const bool clientsStale =
        (activeClients > 0U) && (clientIdleMs > (staleMs * kWebWatchdogClientIdleFactor));

    // A connected but idle WS/HTTP client is normal (dashboard open, no user action).
    // Reboot escalation must only happen when the web loop itself stops progressing.
    if (!loopStale) {
        if (webWatchdogConsecutiveFailures_ > 0U) {
            LOGD("Web watchdog recovered failures=%u loop_age=%lu clients=%u",
                 (unsigned)webWatchdogConsecutiveFailures_,
                 (unsigned long)loopAgeMs,
                 (unsigned)activeClients);
        }
        if (clientsStale) {
            LOGD("Web watchdog client idle (non-fatal) loop_age=%lu clients=%u client_idle=%lu ws_ms=%lu http_ms=%lu",
                 (unsigned long)loopAgeMs,
                 (unsigned)activeClients,
                 (unsigned long)clientIdleMs,
                 (unsigned long)health.lastWsActivityMs,
                 (unsigned long)health.lastHttpActivityMs);
        }
        webWatchdogConsecutiveFailures_ = 0U;
        webWatchdogRebootIssued_ = false;
        return;
    }

    if (webWatchdogConsecutiveFailures_ < 0xFFU) {
        ++webWatchdogConsecutiveFailures_;
    }

    const uint8_t maxFailuresCfg =
        (cfgData_.webWatchdogMaxFailures > 0) ? (uint8_t)cfgData_.webWatchdogMaxFailures : 1U;
    const uint8_t maxFailures =
        (maxFailuresCfg < kWebWatchdogMaxFailuresCap) ? maxFailuresCfg : kWebWatchdogMaxFailuresCap;

    LOGD("Web watchdog fail=%u/%u loop_stale=%d loop_age=%lu stale=%lu clients=%u client_idle=%lu ws_ms=%lu http_ms=%lu",
         (unsigned)webWatchdogConsecutiveFailures_,
         (unsigned)maxFailures,
         (int)loopStale,
         (unsigned long)loopAgeMs,
         (unsigned long)staleMs,
         (unsigned)activeClients,
         (unsigned long)clientIdleMs,
         (unsigned long)health.lastWsActivityMs,
         (unsigned long)health.lastHttpActivityMs);

    if (webWatchdogConsecutiveFailures_ < maxFailures) return;
    if (!cfgData_.webWatchdogAutoReboot) {
        LOGD("Web watchdog reached threshold but auto-reboot is disabled");
        return;
    }
    if (webWatchdogRebootIssued_) return;
    webWatchdogRebootIssued_ = true;

    LOGD("Web watchdog threshold reached, reboot requested");
    if (cmdSvc_ && cmdSvc_->execute) {
        char reply[160] = {0};
        (void)cmdSvc_->execute(cmdSvc_->ctx, "system.reboot", "{}", nullptr, systemActor(), reply, sizeof(reply));
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void SystemMonitorModule::buildHealthJson(char* out, size_t outLen) {
    SystemStatsSnapshot snap{};
    SystemStats::collect(snap);

    /// wifi
    WifiState wst = WifiState::Disabled;
    bool wcon = false;
    char ip[16] = "";
    int rssi = -127;

    if (wifiSvc) {
        wst = wifiSvc->state(wifiSvc->ctx);
        wcon = wifiSvc->isConnected(wifiSvc->ctx);
        wifiSvc->getIP(wifiSvc->ctx, ip, sizeof(ip));
        if (wcon) rssi = WiFi.RSSI();
    }

    snprintf(out, outLen,
        "{"
            "\"upt_ms\":%llu,"
            "\"heap\":{"
                "\"free\":%lu,"
                "\"min_free\":%lu,"
                "\"largest\":%lu,"
                "\"frag\":%u,"
                "\"internal_free\":%lu,"
                "\"internal_min_free\":%lu,"
                "\"internal_largest\":%lu,"
                "\"internal_frag\":%u"
            "}"
        "}",
        (unsigned long long)snap.uptimeMs64,
        (unsigned long)snap.heap.freeBytes,
        (unsigned long)snap.heap.minFreeBytes,
        (unsigned long)snap.heap.largestFreeBlock,
        (unsigned int)snap.heap.fragPercent,
        (unsigned long)snap.heap.internalFreeBytes,
        (unsigned long)snap.heap.internalMinFreeBytes,
        (unsigned long)snap.heap.internalLargestFreeBlock,
        (unsigned int)snap.heap.internalFragPercent
    );
}

void SystemMonitorModule::loop() {
    if (!bootInfoLogged_) {
        bootInfoLogged_ = true;
        logBootInfo();
    }

    const uint32_t now = millis();
    captureTasksIfRequested_();
    sampleTaskStack_(now);
    pollMemoryMetrics_(now);
#if FLOW_WEB_HEAP_FORENSICS
    pollHeapWatch_(now);
    logPendingHeapAllocFailure_();
#endif
    pollMemoryPressureReboot_(now);
    if (cfgStore_) {
        cfgStore_->logNvsWriteSummaryIfDue(now, 60000U);
    }
    pollWebWatchdog_(now);

    // Runtime-only detailed mode increases log cadence, never per-cycle CPU
    // sampling. In normal mode honor configuration with a safe 5s floor.
    const uint32_t configuredPeriodMs =
        (cfgData_.tracePeriodMs > 0) ? (uint32_t)cfgData_.tracePeriodMs : 5000U;
    const uint32_t periodMs = diagnosticsMode_.load() == 1U
        ? 2000U : (configuredPeriodMs < 5000U ? 5000U : configuredPeriodMs);
    const uint32_t heapOffsetMs = periodMs / 3U;
    const uint32_t bufOffsetMs = (periodMs * 2U) / 3U;

    if (traceCycleStartMs_ == 0U) {
        traceCycleStartMs_ = now;
        healthReportedThisCycle_ = false;
        heapLoggedThisCycle_ = false;
        buffersLoggedThisCycle_ = false;
    }

    while ((uint32_t)(now - traceCycleStartMs_) >= periodMs) {
        traceCycleStartMs_ += periodMs;
        healthReportedThisCycle_ = false;
        heapLoggedThisCycle_ = false;
        buffersLoggedThisCycle_ = false;
    }

    const uint32_t cycleElapsedMs = (uint32_t)(now - traceCycleStartMs_);
    if (!healthReportedThisCycle_ && (lastHealthReportMs_ == 0U ||
                                  now - lastHealthReportMs_ >= kMinHealthReportIntervalMs)) {
        sampleSystemHealth_();
        lastHealthReportMs_ = now;
        healthReportedThisCycle_ = true;
    }
    if (!heapLoggedThisCycle_ && cycleElapsedMs >= heapOffsetMs) {
        logHeapStats();
        heapLoggedThisCycle_ = true;
    }
    if (!buffersLoggedThisCycle_ && cycleElapsedMs >= bufOffsetMs) {
        logTrackedBuffers();
        buffersLoggedThisCycle_ = true;
    }

    delayNextCycle(pdMS_TO_TICKS(25));
}

void SystemMonitorModule::pollMemoryMetrics_(uint32_t nowMs)
{
    // [MEM] Low-overhead, always-on memory telemetry. Sample once per second
    // to catch short-lived pressure, summarize every 15 seconds, and report
    // sudden internal RAM drops or threshold crossings immediately.
    static uint32_t lastMemSampleMs = 0U;
    static uint32_t lastMemReportMs = 0U;
    static uint32_t previousInternalFree = 0U;
    static uint32_t windowInternalLow = UINT32_MAX;
    static uint32_t windowLargestLow = UINT32_MAX;
    static bool memSampled = false;
    static bool wasLow = false;
    const uint32_t memNowMs = nowMs;
    if (!memSampled || (uint32_t)(memNowMs - lastMemSampleMs) >= 1000U) {
        lastMemSampleMs = memNowMs;
        constexpr uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        const uint32_t internalFree = (uint32_t)heap_caps_get_free_size(internalCaps);
        const uint32_t internalLargest = (uint32_t)heap_caps_get_largest_free_block(internalCaps);
        const uint32_t internalMinimum = (uint32_t)heap_caps_get_minimum_free_size(internalCaps);
        const uint32_t psramFree = (uint32_t)heap_caps_get_free_size(psramCaps);
        const uint32_t psramLargest = (uint32_t)heap_caps_get_largest_free_block(psramCaps);
        const uint32_t psramMinimum = (uint32_t)heap_caps_get_minimum_free_size(psramCaps);
        // Largest allocatable block / total free memory is a useful
        // fragmentation proxy (not an allocator-internal fragmentation metric).
        const uint32_t internalFragPct = internalFree > 0U
            ? (uint32_t)(100ULL * (internalFree - internalLargest) / internalFree) : 0U;
        const uint32_t psramFragPct = psramFree > 0U
            ? (uint32_t)(100ULL * (psramFree - psramLargest) / psramFree) : 0U;
        const auto webStack = readTaskStack_(taskHealthId(ModuleId::WebInterface, TaskRole::Primary));
        const auto asyncStack = readTaskStack_(taskHealthId(ModuleId::WebInterface, TaskRole::Library));
        const uint32_t stackFreeBytes = webStack.samples ? webStack.freeBytes : 0U;
        const uint32_t asyncStackFreeBytes = asyncStack.samples ? asyncStack.freeBytes : 0U;
        if (internalFree < windowInternalLow) windowInternalLow = internalFree;
        if (internalLargest < windowLargestLow) windowLargestLow = internalLargest;
        const bool low = internalFree < 40000U || internalLargest < 16000U || (webStack.samples && stackFreeBytes < 512U) || (asyncStack.samples && asyncStackFreeBytes < 1024U);
        const bool suddenDrop = memSampled && previousInternalFree > internalFree &&
                                previousInternalFree - internalFree >= 8192U;
        const bool periodic = !memSampled || (uint32_t)(memNowMs - lastMemReportMs) >= 15000U;
        if (periodic || suddenDrop || (low && !wasLow)) {
            const char* tag = low ? "[MEM-LOW]" : (suddenDrop ? "[MEM-DROP]" : "[MEM]");
            LOGD("%s ms=%lu int_free=%lu int_largest=%lu int_frag_pct=%lu int_min=%lu",
                 tag, (unsigned long)memNowMs, (unsigned long)internalFree,
                 (unsigned long)internalLargest, (unsigned long)internalFragPct,
                 (unsigned long)internalMinimum);
            LOGD("%s ms=%lu win_free_min=%lu win_largest_min=%lu psram_free=%lu psram_largest=%lu",
                 tag, (unsigned long)memNowMs, (unsigned long)windowInternalLow,
                 (unsigned long)windowLargestLow, (unsigned long)psramFree, (unsigned long)psramLargest);
            LOGD("%s ms=%lu psram_frag_pct=%lu psram_min=%lu web_stack_free_bytes=%lu async_stack_free_bytes=%lu",
                 tag, (unsigned long)memNowMs, (unsigned long)psramFragPct,
                 (unsigned long)psramMinimum, (unsigned long)stackFreeBytes,
                 (unsigned long)asyncStackFreeBytes);
            if (periodic) {
                lastMemReportMs = memNowMs;
                windowInternalLow = internalFree;
                windowLargestLow = internalLargest;
            }
        }
        previousInternalFree = internalFree;
        wasLow = low;
        memSampled = true;
    }
}

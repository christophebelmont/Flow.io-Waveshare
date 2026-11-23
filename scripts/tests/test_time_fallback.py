"""Exercise clock priority and scheduler continuity through real time-module methods."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from scripts.tests.test_ha_alarm_discovery import function

ROOT = Path(__file__).resolve().parents[2]


class TimeFallbackTests(unittest.TestCase):
    def test_priority_and_scheduler_during_ntp_retries(self):
        module = ROOT / 'src/Modules/Network/TimeModule'
        source = (module / 'TimeModule.cpp').read_text()
        header = (module / 'TimeModule.h').read_text()
        signatures = (
            'void TimeModule::setState(', 'TimeSyncState TimeModule::stateSvc_(',
            'bool TimeModule::isSynced_(', 'bool TimeModule::isExternalRtc_(',
            'void TimeModule::logTimeJump_(',
            'bool TimeModule::setRtcEpoch_(', 'void TimeModule::serviceInternalRtcFallback_(',
            'void TimeModule::serviceNextionFallback_(', 'void TimeModule::tickScheduler_(',
            'uint8_t TimeModule::weekBitFromTm_(', 'uint32_t TimeModule::minuteOfDay_(',
            'bool TimeModule::isWeekdayEnabled_(', 'bool TimeModule::isRecurringTriggerNow_(',
            'bool TimeModule::isRecurringActiveNow_(', 'bool TimeModule::isMonthStartEvent_(',
        )
        bodies = [function(source, signature) for signature in signatures]
        declarations = '\n'.join(body[:body.index('{')].strip().replace('TimeModule::', '') + ';'
                                 for body in bodies)
        types = header[header.index('    struct SchedulerSlotRuntime {'):header.index('    struct SourceSnapshot {')]
        constant_names = ('kNextionRtcFallbackDelayMs', 'kNextionRtcFallbackRetryMs',
                          'kInternalRtcFallbackDelayMs', 'kInternalRtcFallbackRetryMs',
                          'kTimeLargeJumpSeconds')
        constants = '\n'.join(line for line in source.splitlines() if
                              line.startswith('static constexpr uint32_t ') and
                              any(name in line for name in constant_names))
        preamble = r'''
#include "Core/Services/ITime.h"
#include "Core/Services/ITimeScheduler.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include <vector>
#define FLOW_RTC_PCF85063 1
template<class... T> void logNoop(T...) {}
static unsigned warningCount = 0;
template<class... T> void logWarning(T...) { ++warningCount; }
#define LOGI(...) logNoop(__VA_ARGS__)
#define LOGW(...) logWarning(__VA_ARGS__)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static uint32_t tick = 90000;
static uint64_t epoch = 1795177741;
static bool setTimeFails = false;
uint32_t millis() { return tick; }
int fakeSetTime(const timeval* tv, void*) {
    if (setTimeFails) return -1;
    epoch = tv->tv_sec;
    return 0;
}
#define settimeofday fakeSetTime
struct Log { static void setWallClockValid(bool) {} };
struct DataStore { bool ready = false; };
void setTimeReady(DataStore& ds, bool ready) { ds.ready = ready; }
enum class SchedulerEdge : uint8_t { Start, Stop, Trigger };
const char* schedulerEdgeStr(uint8_t) { return "edge"; }
enum class EventId { SchedulerEventTriggered };
enum class ModuleId { Time };
struct SchedulerEventTriggeredPayload {
    uint8_t slot = 0, edge = 0, replayed = 0;
    uint16_t eventId = 0;
    uint64_t epochSec = 0;
    uint16_t activeMask = 0;
};
struct EventBus {
    std::vector<SchedulerEventTriggeredPayload> events;
    bool post(EventId, const void* payload, size_t, ModuleId) {
        events.push_back(*static_cast<const SchedulerEventTriggeredPayload*>(payload));
        return true;
    }
};
struct TimeModule {
    static constexpr uint32_t INVALID_MINUTE_KEY = 0xffffffffU;
    // TYPES
    // DECLARATIONS
    TimeSyncState state = TimeSyncState::WaitingNetwork;
    uint32_t stateTs = 0;
    TimeState timeState_{};
    TimeSource activeSource_ = TimeSource::None;
    unsigned _retryCount = 0, _retryDelayMs = 0;
    bool syncedFromExternalRtc_ = false, syncedFromInternalRtc_ = false;
    bool internalRtcReady_ = true, internalRtcWritePending_ = false, nextionRtcWritePending_ = false;
    uint32_t lastInternalRtcResyncMs_ = 0, lastInternalRtcReadAttemptMs_ = 0, lastNextionRtcReadAttemptMs_ = 0;
    unsigned rtcReads = 0, nextionReads = 0;
    uint64_t rtcEpoch = 1795177735, nextionEpoch = 1795177741;
    bool rtcValid = true, nextionValid = true;
    void* hmiSvc_ = this;
    DataStore store;
    DataStore* dataStore = &store;
    EventBus bus;
    EventBus* eventBus = &bus;
    struct { char tz[4] = "UTC"; } cfgData;
    SchedulerSlotRuntime sched_[TIME_SCHED_MAX_SLOTS]{};
    SchedulerPendingEvent pending[TIME_SCHED_MAX_SLOTS * 2]{};
    SchedulerPendingEvent* schedulerPending_ = pending;
    bool schedInitialized_ = false;
    uint64_t lastSchedulerEvalEpochSec_ = 0;
    uint16_t activeMaskValue_ = 0;
    int schedMux_ = 0;
    time_t nowEpoch_() const { return epoch; }
    bool isTimePlausible_(uint64_t value) { return value >= 1767225600 && value < 4102444800; }
    uint64_t minPlausibleEpochSec_() { return 1767225600; }
    static constexpr uint64_t kTimeMaxPlausibleEpochSec = 4102444800;
    const char* sourceName_(TimeSource) { return "source"; }
    const char* qualityName_(TimeQuality) { return "quality"; }
    template<class... T> void recordSource_(T...) {}
    template<class... T> void noteGoodTime_(T...) {}
    void setActiveSource_(TimeSource source, TimeQuality) { activeSource_ = source; }
    bool ensureInternalRtcInit_() { return internalRtcReady_; }
    bool internalRtcBatteryPresent_() { return true; }
    bool internalRtcReadEpoch_(uint64_t& out) { ++rtcReads; out = rtcEpoch; return rtcValid; }
    bool nextionRtcReadEpoch_(uint64_t& out) { ++nextionReads; out = nextionEpoch; return nextionValid; }
    bool formatLocalTime_(char*, size_t) { return true; }
    uint16_t activeMask_() const { return activeMaskValue_; }
};
'''
        scenarios = r'''
int main() {
    setenv("TZ", "UTC", 1); tzset();
    // A retained system time is not a validated reference at boot. After the
    // first accepted source, warn only about corrections actually applied.
    {
        TimeModule m;
        epoch = 1795441361;
        warningCount = 0;
        m.rtcEpoch = 1795441356;
        m.serviceInternalRtcFallback_(tick);
        assert(epoch == 1795441356 && m.isSynced_());
        assert(warningCount == 0);
        assert(m.setRtcEpoch_(1795441351, TimeSource::InternalRtc, TimeQuality::RtcTrusted, "resync"));
        assert(epoch == 1795441351 && warningCount == 1);
        setTimeFails = true;
        assert(!m.setRtcEpoch_(1795441346, TimeSource::InternalRtc, TimeQuality::RtcTrusted, "failed"));
        assert(epoch == 1795441351 && warningCount == 1);
        setTimeFails = false;
        assert(m.setRtcEpoch_(epoch + 1, TimeSource::Ntp, TimeQuality::NtpSynced, "ntp"));
        assert(warningCount == 1);
        assert(m.setRtcEpoch_(epoch + kTimeLargeJumpSeconds, TimeSource::Ntp, TimeQuality::NtpSynced, "ntp"));
        assert(warningCount == 2);
    }
    // Every NTP transport state retains every accepted clock. A lower-priority
    // fallback must never sample Nextion or replace NTP with the board RTC.
    for (auto state : {TimeSyncState::WaitingNetwork, TimeSyncState::Syncing,
                       TimeSyncState::ErrorWait, TimeSyncState::Synced, TimeSyncState::Disabled}) {
        for (auto source : {TimeSource::Ntp, TimeSource::InternalRtc, TimeSource::NextionManual}) {
            TimeModule m;
            assert(m.setRtcEpoch_(1795177735, source, TimeQuality::RtcTrusted, "test"));
            m.setState(state);
            m.serviceNextionFallback_(tick);
            assert(m.nextionReads == 0 && m.activeSource_ == source);
            assert(m.isSynced_() && m.store.ready && m.stateSvc_() == TimeSyncState::Synced);
            assert(m.isExternalRtc_() == (source != TimeSource::Ntp));
            if (source != TimeSource::NextionManual) {
                m.serviceInternalRtcFallback_(tick);
                assert(m.rtcReads == 0 && m.activeSource_ == source);
            }
        }
    }
    // Boot still falls back when no accepted clock exists, and a board RTC can
    // supersede a Nextion fallback once it becomes available.
    {
        TimeModule m;
        m.setState(TimeSyncState::Syncing);
        assert(!m.store.ready && !m.isSynced_());
        m.rtcValid = false;
        m.serviceInternalRtcFallback_(tick);
        m.serviceNextionFallback_(tick);
        assert(m.activeSource_ == TimeSource::NextionManual && m.nextionReads == 1);
        tick += 10001;
        m.rtcValid = true;
        m.serviceInternalRtcFallback_(tick);
        assert(m.activeSource_ == TimeSource::InternalRtc && m.rtcReads == 2);
    }
    // An active filtration window replays once at boot. Retries and later time
    // corrections preserve the mask; the stop edge still fires during retry.
    {
        TimeModule m;
        auto& slot = m.sched_[4];
        slot.used = true; slot.def.slot = 4; slot.def.eventId = 8450;
        slot.def.hasEnd = true; slot.def.startHour = 8; slot.def.endHour = 18;
        m.tickScheduler_();
        assert(m.bus.events.empty());
        assert(m.setRtcEpoch_(1795177735, TimeSource::InternalRtc, TimeQuality::RtcTrusted, "boot"));
        m.tickScheduler_();
        assert(m.bus.events.size() == 1 && m.bus.events[0].replayed == 1);
        assert(m.activeMaskValue_ == 0x0010);
        for (auto state : {TimeSyncState::WaitingNetwork, TimeSyncState::Syncing,
                           TimeSyncState::ErrorWait, TimeSyncState::Synced}) {
            m.setState(state); m.tickScheduler_();
            assert(m.bus.events.size() == 1 && m.activeMaskValue_ == 0x0010);
        }
        assert(m.setRtcEpoch_(1795177741, TimeSource::Ntp, TimeQuality::NtpSynced, "ntp"));
        m.tickScheduler_();
        assert(m.bus.events.size() == 1);
        m.setState(TimeSyncState::Syncing);
        epoch += 86400 / 2;
        m.tickScheduler_();
        assert(m.bus.events.size() == 2 && !m.bus.events[1].replayed);
        assert(m.bus.events[1].edge == static_cast<uint8_t>(SchedulerEdge::Stop));
        assert(m.activeMaskValue_ == 0);
    }
}
'''
        code = constants + preamble.replace('// TYPES', types).replace('// DECLARATIONS', declarations)
        # Constants use the fixed-width integer types from the preamble.
        code = '#include <cstdint>\n#define FLOW_RTC_PCF85063 1\n#define TIME_LARGE_JUMP_SECONDS 300\n' + code + '\n'.join(bodies) + scenarios
        with tempfile.TemporaryDirectory(prefix='flow-time-fallback-') as tmp:
            cpp = Path(tmp) / 'time.cpp'
            cpp.write_text(code)
            exe = Path(tmp) / 'time'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-Isrc', str(cpp), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

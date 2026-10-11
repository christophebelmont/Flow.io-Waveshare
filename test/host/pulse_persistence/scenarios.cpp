#include "fixture.h"
int main() {
    { // A request arriving during acquisition is newer than the cycle timestamp.
        IOModule io;
        const uint32_t cycleStarted = testNow;
        testNow += 10;
        io.drivers[0].raw = 42;
        io.request();
        io.servicePulseRequest_(cycleStarted);
        assert(io.pulseRequestState_.load() == IOModule::PulseRequestState::Saving);
        assert(io.writes == 1 && io.queued.count[0] == 42);
        io.complete(); io.step();
        assert(io.pulseRequestResult_ == IO_OK);
        testNow = 0;
    }
    { // Periodic frequency unchanged, then forced save reads the latest hardware count.
        IOModule io;
        io.drivers[0].raw = 53;
        io.processDigitalInputDefinition_(0, testNow);
        io.checkpointPulses_(100);
        assert(io.writes == 0);
        io.checkpointPulses_(PulseCheckpoint::PeriodMs);
        assert(io.writes == 1 && io.queued.count[0] == 53);
        io.drivers[0].raw = 59;
        io.request(); io.step(); // Older receipt must finish first.
        assert(io.writes == 1);
        io.complete(); io.step();
        assert(io.writes == 2 && io.queued.count[0] == 59);
        assert(io.pulseRequestState_.load() == IOModule::PulseRequestState::Saving);
        io.complete(); io.step();
        assert(io.pulseRequestResult_ == IO_OK);
        assert(io.loadPulseCheckpoint_());
        assert(io.pulsePersisted_.count[0] == 59);
    }
    { // Reset only the requested input, once, and preserve subsequent pulses.
        IOModule io;
        io.drivers[0].raw = 100; io.drivers[1].raw = 71;
        io.processDigitalInputDefinition_(0, testNow); io.processDigitalInputDefinition_(1, testNow);
        io.request(64); io.step();
        assert(io.queued.count[0] == 0 && io.queued.count[1] == 71);
        assert(io.queued.generation[0] == 1);
        io.drivers[0].raw = 103; io.processDigitalInputDefinition_(0, testNow);
        io.step(); assert(io.slots[0].pulse.count == 3);
        io.complete(); io.step();
        assert(io.pulseRequestResult_ == IO_OK);
        assert(io.loadPulseCheckpoint_());
        assert(io.pulsePersisted_.count[0] == 0 && io.pulsePersisted_.generation[0] == 1);
        assert(io.slots[0].pulse.count == 3);
    }
    { // Failed persistence never reports success and remains eligible for retry.
        IOModule io;
        io.request(64); io.step(); io.complete(false); io.step();
        assert(io.pulseRequestResult_ == IO_ERR_PERSISTENCE);
        io.checkpointPulses_(testNow);
        assert(io.pulseRetry_);
        io.request(); io.step(); io.complete(); io.step();
        assert(io.pulseRequestResult_ == IO_OK);
    }
    {
        IOModule io;
        io.enqueueOk = false; io.request(64); io.step();
        assert(io.pulseRequestResult_ == IO_ERR_PERSISTENCE && io.writes == 0);
    }
    { // No active acquisition must not prevent firmware recovery or erase stored counters.
        IOModule io;
        io.runtimeReady_ = false; io.pulseStorageReady_ = false;
        for (auto& slot : io.slots) slot.pulse.initialized = false;
        io.request(); io.step();
        assert(io.pulseRequestResult_ == IO_OK && io.writes == 0);
        io.request(64); io.step(); assert(io.pulseRequestResult_ == IO_ERR_NOT_READY);
    }
    {
        IOModule io;
        io.drivers[0].readable = false; io.request(); io.step();
        assert(io.pulseRequestResult_ == IO_ERR_HW && io.writes == 0);
        io.drivers[0].readable = true;
        io.slots[0].inDef.mode = 0; io.request(64); io.step();
        assert(io.pulseRequestResult_ == IO_ERR_NOT_READY && io.writes == 0);
    }
    { // Unstarted requests expire rather than reset unexpectedly after the caller times out.
        IOModule io;
        testTick = {};
        assert(io.resetCounter_(64) == IO_ERR_TIMEOUT);
        io.step();
        assert(io.pulseRequestResult_ == IO_ERR_TIMEOUT && io.writes == 0);
        assert(io.slots[0].pulse.generation == 0);
    }
    { // Bounded caller waits for flash, and concurrent requests cannot replace its payload.
        IOModule io;
        bool busyChecked = false;
        testTick = [&] {
            if (!busyChecked) { busyChecked = true; assert(io.resetCounter_(65) == IO_ERR_BUSY); }
            io.step();
            if (io.pulseReceipt_.status.load() == PersistenceReceipt::Pending) io.complete();
        };
        assert(io.resetCounter_(64) == IO_OK);
        assert(io.queued.generation[0] == 1 && io.queued.generation[1] == 0);
        testTick = {};
    }
    { // Timeout during a write keeps receipt memory alive and prevents overlapping operations.
        IOModule io;
        testTick = [&] { io.step(); };
        assert(io.resetCounter_(64) == IO_ERR_TIMEOUT);
        assert(io.resetCounter_(65) == IO_ERR_BUSY);
        io.complete(); io.step();
        assert(io.pulseRequestResult_ == IO_OK);
        testTick = {};
    }
    { // Missing new-format checkpoint starts at zero; corrupt data does not silently reset.
        IOModule io;
        assert(io.loadPulseCheckpoint_());
        assert(io.pulsePersisted_.count[0] == 0);
        io.store.bytes[0] = 0;
        assert(!io.loadPulseCheckpoint_());
    }
}

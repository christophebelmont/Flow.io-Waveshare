int main() {
    PoolDeviceModule original;
    original.boot();
    assert(original.slots_[0].desired.setpoint == 2900 && !original.slots_[0].desired.running);
    assert(original.storage.writes == 2); // One initial default per RPM equipment.
    PoolDeviceTarget target{true, 1800};
    assert(original.svcSetTargetImpl_(0, &target) == POOLDEV_SVC_OK);
    assert(original.storage.prefs.rpm.at("act0_rpm") == 1800);
    assert(original.slots_[0].desired.running && original.slots_[0].desired.setpoint == 1800);
    const auto written = original.storage.writes;
    for (unsigned i=0; i<20; ++i) {
        assert(original.svcSetRunningImpl_(0,0) == POOLDEV_SVC_OK);
        assert(original.svcSetRunningImpl_(0,1) == POOLDEV_SVC_OK);
        original.slots_[0].feedback.observed.setpoint = 2900;
        original.syncConfigurationSetpoint_(0);
    }
    assert(original.slots_[0].desired.setpoint == 1800 && original.storage.writes == written);
    assert(original.svcSetRunningImpl_(0,0) == POOLDEV_SVC_OK);
    target = {false, 2100};
    assert(original.svcSetTargetImpl_(0, &target) == POOLDEV_SVC_OK);
    assert(!original.slots_[0].desired.running);
    assert(original.storage.prefs.rpm.at("act0_rpm") == 2100);

    PoolDeviceModule reboot;
    reboot.storage.prefs = original.storage.prefs;
    reboot.boot();
    assert(reboot.storage.writes == 0);
    assert(reboot.slots_[0].desired.setpoint == 2100 && !reboot.slots_[0].desired.running);
    assert(reboot.slots_[1].desired.setpoint == 2900); // Per-slot independence.
    assert(reboot.svcSetRunningImpl_(0,1) == POOLDEV_SVC_OK);
    assert(reboot.slots_[0].desired.setpoint == 2100);

    // Failed durable writes must not apply an unpersisted speed.
    reboot.storage.fail = true;
    target = {true, 2300};
    assert(reboot.svcSetTargetImpl_(0, &target) == POOLDEV_SVC_ERR_IO);
    assert(reboot.slots_[0].requestedRpm == 2100 && reboot.slots_[0].desired.setpoint == 2100);
    assert(reboot.storage.prefs.rpm.at("act0_rpm") == 2100);
    assert(reboot.svcSetRunningImpl_(0,0) == POOLDEV_SVC_OK); // Safety stop needs no NVS write.
    assert(!reboot.slots_[0].desired.running);
    reboot.storage.fail = false;
    assert(reboot.svcSetTargetImpl_(0, &target) == POOLDEV_SVC_OK);
    assert(reboot.slots_[0].requestedRpm == 2300);

    // Native and JSON configuration use the same range/type validation.
    assert(!reboot.storage.set(reboot.cfgRequestedRpmVar_[0], 1100.0f));
    assert(!reboot.storage.set(reboot.cfgRequestedRpmVar_[0], NAN));
    for (const char* patch : {R"({"pdm/pd0":{"requested_rpm":"1800"}})",
                             R"({"pdm/pd0":{"requested_rpm":3000}})",
                             R"({"pdm/pd0":{"requested_rpm":-1}})"}) {
        StaticJsonDocument<256> doc;
        assert(!deserializeJson(doc, patch));
        assert(!reboot.validateSpeedConfiguration_(ConfigCandidate(doc.as<JsonObjectConst>())));
    }
    StaticJsonDocument<256> valid;
    assert(!deserializeJson(valid, R"({"pdm/pd0":{"requested_rpm":1750}})"));
    assert(reboot.validateSpeedConfiguration_(ConfigCandidate(valid.as<JsonObjectConst>())));
    assert(reboot.storage.set(reboot.cfgRequestedRpmVar_[0], 1750.0f));
    reboot.syncConfigurationSetpoint_(0);
    assert(reboot.slots_[0].desired.setpoint == 1750);

    // Invalid stored data / a changed driver range is normalized once at boot.
    PoolDeviceModule invalid;
    invalid.storage.prefs.rpm["act0_rpm"] = 4000;
    invalid.storage.prefs.rpm["act1_rpm"] = NAN;
    invalid.boot();
    assert(invalid.slots_[0].desired.setpoint == 2900 && invalid.slots_[1].desired.setpoint == 2900);
    assert(invalid.storage.writes == 2);
    invalid.storage.fail = true;
    assert(!invalid.restoreRequestedSpeed_(0));
    PoolDeviceModule missingKey;
    missingKey.slots_[0].requestedRpm = 2900;
    assert(missingKey.restoreRequestedSpeed_(0));
    assert(missingKey.storage.prefs.rpm.at("act0_rpm") == 2900 && missingKey.storage.writes == 1);
    puts("Requested RPM persistence, reboot, stop, validation and failure rollback passed");
}

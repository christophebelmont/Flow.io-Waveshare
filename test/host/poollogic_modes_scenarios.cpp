
static void modeMatrix()
{
    for (uint8_t treatment = 0; treatment <= 3; ++treatment) {
        PoolLogicModule automatic;
        automatic.window = true;
        automatic.levelLow = true;
        automatic.prepare(PoolOperatingMode::Automatic, treatment);
        automatic.tick();
        automatic.tick(11 * 60000);
        automatic.tick();
        assert(automatic.guided[automatic.filtrationDeviceSlot_]);
        assert(automatic.guided[automatic.phPumpDeviceSlot_]);
        assert(automatic.guided[automatic.fillingDeviceSlot_]);
        assert(automatic.guided[automatic.orpPumpDeviceSlot_] == (treatment == 0 || treatment == 2));
        assert(automatic.guided[automatic.swgDeviceSlot_] == (treatment == 1));

        PoolLogicModule manual;
        manual.air = -10; manual.levelLow = true; manual.window = true; manual.heaterAutoMode_ = true;
        manual.o2PendingMl_ = 500;
        manual.prepare(PoolOperatingMode::Manual, treatment);
        manual.tick(15 * 60000);
        for (auto on : manual.guided) assert(!on);
        manual.guided[manual.filtrationDeviceSlot_] = manual.actual[manual.filtrationDeviceSlot_] = true;
        manual.tick(15 * 60000);
        assert(!manual.guided[manual.phPumpDeviceSlot_]);
        assert(!manual.guided[manual.orpPumpDeviceSlot_]);
        assert(!manual.guided[manual.heaterDeviceSlot_]);
        assert(PoolLogicModule::serviceManualControlAllowed_(&manual, manual.phPumpDeviceSlot_));

        PoolLogicModule winter;
        winter.air = -10; winter.levelLow = true; winter.heaterAutoMode_ = true; winter.o2PendingMl_ = 500;
        winter.prepare(PoolOperatingMode::Winter, treatment);
        winter.tick(15 * 60000);
        assert(winter.guided[winter.filtrationDeviceSlot_]);
        for (auto slot : {winter.phPumpDeviceSlot_, winter.orpPumpDeviceSlot_, winter.swgDeviceSlot_,
                winter.robotDeviceSlot_, winter.heaterDeviceSlot_, winter.fillingDeviceSlot_}) assert(!winter.guided[slot]);
        assert(!PoolLogicModule::serviceManualControlAllowed_(&winter, winter.phPumpDeviceSlot_));
        assert(winter.o2PendingMl_ == 500);
        winter.air = 1; winter.tick();
        assert(winter.guided[winter.filtrationDeviceSlot_]); // Freeze hold after startup.
        winter.air = 10; winter.tick();
        assert(!winter.guided[winter.filtrationDeviceSlot_]);
        winter.window = true; winter.tick();
        assert(winter.guided[winter.filtrationDeviceSlot_]); // Winter retains the normal schedule.
        winter.pressureAlarm = true; winter.tick();
        assert(!winter.guided[winter.filtrationDeviceSlot_]);
    }
}

static void runningTransitions()
{
    PoolLogicModule p;
    p.window = true;
    p.prepare(PoolOperatingMode::Automatic);
    p.tick(); p.tick(6 * 60000); p.tick();
    assert(p.guided[p.phPumpDeviceSlot_] && p.guided[p.orpPumpDeviceSlot_]);
    p.forced[p.phPumpDeviceSlot_] = true;
    p.phCorrectionType_ = uint8_t(PoolPhCorrection::Disabled);
    p.tick();
    assert(!p.guided[p.phPumpDeviceSlot_] && !p.forced[p.phPumpDeviceSlot_]);
    assert(p.guided[p.filtrationDeviceSlot_]);
    p.tick();
    assert(p.guided[p.orpPumpDeviceSlot_]);
    p.operatingMode_ = uint8_t(PoolOperatingMode::Winter);
    p.forced[p.orpPumpDeviceSlot_] = true;
    p.tick();
    assert(!p.guided[p.orpPumpDeviceSlot_] && !p.forced[p.orpPumpDeviceSlot_]);
    assert(!p.phPidEnabled_ && !p.orpPidEnabled_);
    p.operatingMode_ = uint8_t(PoolOperatingMode::Manual);
    p.tick();
    assert(p.guided[p.filtrationDeviceSlot_]); // Manu does not create a new filtration demand.
    p.guided[p.filtrationDeviceSlot_] = p.actual[p.filtrationDeviceSlot_] = false;
    p.tick();
    assert(!p.guided[p.filtrationDeviceSlot_]);
    p.operatingMode_ = uint8_t(PoolOperatingMode::Automatic);
    p.phCorrectionType_ = uint8_t(PoolPhCorrection::Minus);
    p.tick(); p.tick();
    assert(!p.guided[p.phPumpDeviceSlot_]); // Fresh circulation delay after restart.
    p.tick(6 * 60000); p.tick();
    assert(p.guided[p.phPumpDeviceSlot_]);
    p.analogValid = false; p.tick();
    assert(!p.guided[p.phPumpDeviceSlot_] && !p.guided[p.orpPumpDeviceSlot_]);
}

static void phDirectionAndDisabledTreatment()
{
    PoolLogicModule p;
    p.window = true;
    p.ph = 6;
    p.phCorrectionType_ = uint8_t(PoolPhCorrection::Plus);
    p.operatingMode_ = uint8_t(PoolOperatingMode::Automatic);
    p.disinfectionType_ = PoolLogicModule::DisinfectionDisabled;
    p.tick(); p.tick(); p.tick(6 * 60000); p.tick();
    assert(p.guided[p.phPumpDeviceSlot_]);
    assert(!p.guided[p.orpPumpDeviceSlot_] && !p.guided[p.swgDeviceSlot_]);
    p.phCorrectionType_ = uint8_t(PoolPhCorrection::Minus);
    p.tick(); p.tick();
    assert(!p.guided[p.phPumpDeviceSlot_]);
}

static void persistedMigration()
{
    for (bool enabled : {false, true}) for (bool automatic : {false, true}) for (bool winter : {false, true}) {
        PoolLogicModule p;
        auto& nvs = p.storage.records;
        nvs[NvsKeys::PoolLogic::Enabled] = enabled;
        nvs[NvsKeys::PoolLogic::AutoMode] = automatic;
        nvs[NvsKeys::PoolLogic::WinterMode] = winter;
        nvs[NvsKeys::PoolLogic::PhAutoMode] = true;
        nvs[NvsKeys::PoolLogic::PhDosePlus] = true;
        nvs[NvsKeys::PoolLogic::OrpAutoMode] = true;
        assert(p.migrateModes_());
        assert(p.operatingMode() == (!enabled || !automatic ? PoolOperatingMode::Manual : winter ? PoolOperatingMode::Winter : PoolOperatingMode::Automatic));
        assert(p.phCorrection() == PoolPhCorrection::Plus);
        assert(!nvs.count(NvsKeys::PoolLogic::Enabled));
        p.operatingMode_ = uint8_t(PoolOperatingMode::Manual);
        p.storage.set(p.operatingModeVar_, uint8_t(PoolOperatingMode::Manual));
        assert(p.migrateModes_());
        assert(p.operatingMode() == PoolOperatingMode::Manual);
    }
    for (uint8_t treatment = 0; treatment <= 3; ++treatment) {
        PoolLogicModule p;
        p.disinfectionType_ = treatment;
        assert(p.migrateModes_());
        assert(p.disinfectionType_ == (treatment == 0 ? 3 : treatment));
        assert(p.phCorrection() == PoolPhCorrection::Disabled);
    }
    for (int failure = 0; failure < 4; ++failure) {
        PoolLogicModule first;
        first.storage.records = {{NvsKeys::PoolLogic::Enabled,1}, {NvsKeys::PoolLogic::AutoMode,1},
            {NvsKeys::PoolLogic::PhAutoMode,1}, {NvsKeys::PoolLogic::OrpAutoMode,1}};
        first.storage.writesBeforeFailure = failure;
        assert(!first.migrateModes_());
        assert(first.storage.records.count(NvsKeys::PoolLogic::Enabled));
        PoolLogicModule reboot;
        reboot.storage.records = first.storage.records;
        reboot.storage.loadPersistentVar(reboot.operatingModeVar_);
        reboot.storage.loadPersistentVar(reboot.phCorrectionTypeVar_);
        reboot.storage.loadPersistentVar(reboot.disinfectionTypeVar_);
        assert(reboot.migrateModes_());
        assert(reboot.operatingMode() == PoolOperatingMode::Automatic);
        assert(reboot.phCorrection() == PoolPhCorrection::Minus);
    }
}

static void corruptStoredModes()
{
    PoolLogicModule p;
    p.storage.records[NvsKeys::PoolLogic::ModesMigrated] = 1;
    p.storage.records[NvsKeys::PoolLogic::OperatingMode] = 255;
    p.storage.records[NvsKeys::PoolLogic::PhCorrectionType] = 255;
    p.storage.loadPersistentVar(p.operatingModeVar_);
    p.storage.loadPersistentVar(p.phCorrectionTypeVar_);
    assert(p.migrateModes_());
    assert(p.operatingMode() == PoolOperatingMode::Manual);
    assert(p.phCorrection() == PoolPhCorrection::Disabled);
}

static void leasesAndValidation()
{
    PoolLogicModule p;
    p.forced[p.filtrationDeviceSlot_] = true;
    p.prepare(PoolOperatingMode::Automatic);
    assert(p.forced[p.filtrationDeviceSlot_] && p.releases == 0); // Compatible lease survives boot.
    p.operatingMode_ = uint8_t(PoolOperatingMode::Winter);
    p.failRelease = true;
    p.pressureAlarm = true;
    p.guided[p.filtrationDeviceSlot_] = p.actual[p.filtrationDeviceSlot_] = true;
    p.tick();
    assert(!p.guided[p.filtrationDeviceSlot_]); // Pressure remains authoritative during failed persistence.
    assert(p.appliedMode_ != p.operatingMode_);
    assert(!PoolLogicModule::serviceManualControlAllowed_(&p,p.phPumpDeviceSlot_));
    p.failRelease = false; p.tick();
    assert(p.appliedMode_ == p.operatingMode_ && !p.forced[p.filtrationDeviceSlot_]);
    for (const char* json : {"{\"poollogic/modes\":{\"operating_mode\":3}}", "{\"poollogic/modes\":{\"operating_mode\":-1}}", "{\"poollogic/modes\":{\"operating_mode\":true}}", "{\"poollogic/ph\":{\"correction_type\":3}}"}) {
        StaticJsonDocument<256> doc;
        assert(!deserializeJson(doc,json));
        assert(!PoolLogicModule::validateModes_(&p, ConfigCandidate(doc.as<JsonObjectConst>())));
    }
}

static void modeCommandContract()
{
    PoolLogicModule p;
    char reply[256];
    for (const char* args : {"{\"value\":0}", "{\"value\":1}", "{\"value\":2}"}) {
        const CommandRequest req{"poollogic.operating_mode.set", nullptr, args};
        assert(p.cmdOperatingModeSet_(req, reply, sizeof(reply)));
    }
    const CommandRequest nested{"poollogic.operating_mode.set",
        "{\"cmd\":\"poollogic.operating_mode.set\",\"args\":{\"value\":2}}", nullptr};
    assert(p.cmdOperatingModeSet_(nested, reply, sizeof(reply)));
    assert(p.operatingMode() == PoolOperatingMode::Winter);
    const auto saved = p.operatingMode_;
    for (const char* args : {"{\"value\":3}", "{\"value\":-1}", "{\"value\":true}", "{\"value\":\"Auto\"}", "{\"value\":1.5}", "{}"}) {
        const CommandRequest req{"poollogic.operating_mode.set", nullptr, args};
        assert(!p.cmdOperatingModeSet_(req, reply, sizeof(reply)));
        assert(p.operatingMode_ == saved);
    }
    p.storage.writesBeforeFailure = 0;
    const CommandRequest req{"poollogic.operating_mode.set", nullptr, "{\"value\":0}"};
    assert(!p.cmdOperatingModeSet_(req, reply, sizeof(reply)));
    assert(p.operatingMode_ == saved);
}

int main()
{
    modeMatrix(); runningTransitions(); phDirectionAndDisabledTreatment(); persistedMigration(); leasesAndValidation(); modeCommandContract(); corruptStoredModes();
}

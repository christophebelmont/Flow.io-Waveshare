#include "IOModule.h"
#include "IOConfigDescriptorStorage.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::IOModule)
#include "Core/ModuleLog.h"

void IOModule::prepareBindingMigration_(ConfigStore& cfg)
{
    uint8_t bindingSchema = 0;
    size_t bindingSchemaSize = 0;
    bindingMigrationPending_ = !cfg.readRuntimeBlob("io_bind_v", &bindingSchema, sizeof(bindingSchema), &bindingSchemaSize)
        || bindingSchemaSize != sizeof(bindingSchema) || bindingSchema != 1;
    legacyBindingSlots_ = 0;

    for (uint8_t i = 0; i < ANALOG_CFG_SLOTS; ++i) {
        IOConfigDescriptorStorage::AnalogSlot& vars = configDescriptors_->analog[i];
        bool hasPrimary = false, hasSecondary = false, hasC0 = false, hasC1 = false;
        if (bindingMigrationPending_ && cfg.persistentKeyExists(vars.bindingKey, hasPrimary) &&
            cfg.persistentKeyExists(vars.secondaryBindingKey, hasSecondary) &&
            cfg.persistentKeyExists(vars.c0Key, hasC0) && cfg.persistentKeyExists(vars.c1Key, hasC1) &&
            !hasSecondary && (hasPrimary || hasC0 || hasC1)) {
            const auto& legacy = analogSlots_[i].def.legacyDefault;
            if (legacy.port != IO_PORT_INVALID) {
                analogCfg_[i].binding.primaryPort = legacy.port;
                analogCfg_[i].c0 = legacy.c0;
                analogCfg_[i].c1 = legacy.c1;
            }
            legacyBindingSlots_ |= uint32_t(1) << i;
            analogCfg_[i].binding.secondaryPort = IO_PORT_INVALID;
        }
    }

}

void IOModule::persistBindingMigration_(ConfigStore& cfg)
{
    if (bindingMigrationPending_) {
        bool persisted = true;
        for (uint8_t i = 0; i < ANALOG_CFG_SLOTS; ++i) {
            if (!(legacyBindingSlots_ & (uint32_t(1) << i))) continue;
            auto& config = analogCfg_[i];
            // Commit the old primary/calibration before the new field; a power loss
            // leaves a restartable migration. The schema marker is written last.
            persisted = cfg.persistRegisteredValue(&config.binding.primaryPort) && persisted;
            persisted = cfg.persistRegisteredValue(&config.c0) && persisted;
            persisted = cfg.persistRegisteredValue(&config.c1) && persisted;
            if (persisted) persisted = cfg.persistRegisteredValue(&config.binding.secondaryPort);
        }
        const uint8_t schema = 1;
        if (persisted) persisted = cfg.writeRuntimeBlob("io_bind_v", &schema, sizeof(schema));
        if (!persisted) LOGE("Binding schema migration could not be persisted; retry at next boot");
    }
}

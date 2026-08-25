#pragma once
/**
 * @file RuntimeUi.h
 * @brief Compact runtime UI exposure interfaces for the local Waveshare UI.
 *
 * Design goals:
 * - no runtime manifest stored in application RAM
 * - no duplicated runtime values in a second cache
 * - no dynamic allocation in the runtime read path
 * - routing by compact numeric module/value identifiers only
 */

#include <stddef.h>
#include <stdint.h>

#include "Core/ModuleId.h"

using RuntimeUiId = uint16_t;

constexpr RuntimeUiId kRuntimeUiModuleStride = 100U;

constexpr RuntimeUiId makeRuntimeUiId(ModuleId moduleId, uint8_t valueId)
{
    return (RuntimeUiId)((RuntimeUiId)moduleIdIndex(moduleId) * kRuntimeUiModuleStride + (RuntimeUiId)valueId);
}

constexpr uint8_t runtimeUiModuleId(RuntimeUiId runtimeId)
{
    return (uint8_t)(runtimeId / kRuntimeUiModuleStride);
}

constexpr uint8_t runtimeUiValueId(RuntimeUiId runtimeId)
{
    return (uint8_t)(runtimeId % kRuntimeUiModuleStride);
}

constexpr bool isValidRuntimeUiId(RuntimeUiId runtimeId)
{
    const uint8_t moduleId = runtimeUiModuleId(runtimeId);
    const uint8_t valueId = runtimeUiValueId(runtimeId);
    return moduleId > 0U && moduleId < kModuleIdCount && valueId > 0U;
}

enum class RuntimeUiWireType : uint8_t {
    NotFound = 0,
    Unavailable = 1,
    Bool = 2,
    Int32 = 3,
    UInt32 = 4,
    Float32 = 5,
    Enum = 6,
    String = 7,
};

class IRuntimeUiWriter {
public:
    virtual ~IRuntimeUiWriter() = default;

    virtual bool writeBool(RuntimeUiId runtimeId, bool value) = 0;
    virtual bool writeI32(RuntimeUiId runtimeId, int32_t value) = 0;
    virtual bool writeU32(RuntimeUiId runtimeId, uint32_t value) = 0;
    virtual bool writeF32(RuntimeUiId runtimeId, float value) = 0;
    virtual bool writeEnum(RuntimeUiId runtimeId, uint8_t value) = 0;
    virtual bool writeString(RuntimeUiId runtimeId, const char* value) = 0;
    virtual bool writeNotFound(RuntimeUiId runtimeId) = 0;
    virtual bool writeUnavailable(RuntimeUiId runtimeId) = 0;
};

class IRuntimeUiValueProvider {
public:
    virtual ~IRuntimeUiValueProvider() = default;

    virtual ModuleId runtimeUiProviderModuleId() const = 0;
    virtual bool writeRuntimeUiValue(uint8_t valueId, IRuntimeUiWriter& writer) const = 0;
};

class RuntimeUiRegistry {
public:
    bool registerProvider(const IRuntimeUiValueProvider* provider);
    const IRuntimeUiValueProvider* providerForModule(ModuleId moduleId) const;

private:
    // Fixed table indexed by ModuleId keeps DRAM overhead low and predictable.
    const IRuntimeUiValueProvider* providers_[kModuleIdCount] = {};
};

class RuntimeUiBinaryWriter final : public IRuntimeUiWriter {
public:
    RuntimeUiBinaryWriter(uint8_t* out, size_t capacity);

    bool writeBool(RuntimeUiId runtimeId, bool value) override;
    bool writeI32(RuntimeUiId runtimeId, int32_t value) override;
    bool writeU32(RuntimeUiId runtimeId, uint32_t value) override;
    bool writeF32(RuntimeUiId runtimeId, float value) override;
    bool writeEnum(RuntimeUiId runtimeId, uint8_t value) override;
    bool writeString(RuntimeUiId runtimeId, const char* value) override;
    bool writeNotFound(RuntimeUiId runtimeId) override;
    bool writeUnavailable(RuntimeUiId runtimeId) override;

    size_t length() const { return length_; }
    uint8_t recordCount() const { return recordCount_; }
    bool overflowed() const { return overflowed_; }

private:
    bool writeHeader_(RuntimeUiId runtimeId, RuntimeUiWireType type, size_t payloadLen);
    bool appendU8_(uint8_t value);
    bool appendU16_(uint16_t value);
    bool appendU32_(uint32_t value);
    bool appendBytes_(const uint8_t* data, size_t len);

    uint8_t* out_ = nullptr;
    size_t capacity_ = 0;
    size_t length_ = 0;
    uint8_t recordCount_ = 0;
    bool overflowed_ = false;
};

class RuntimeUiService {
public:
    explicit RuntimeUiService(RuntimeUiRegistry& registry)
        : registry_(registry)
    {
    }

    bool readValues(const RuntimeUiId* ids,
                    uint8_t count,
                    uint8_t* out,
                    size_t outCapacity,
                    size_t& outLen) const;

private:
    RuntimeUiRegistry& registry_;
};

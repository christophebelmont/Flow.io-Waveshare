/**
 * @file RuntimeUi.cpp
 * @brief Compact runtime UI registry/service implementation.
 */

#include "Core/RuntimeUi.h"

#include <string.h>

bool RuntimeUiRegistry::registerProvider(const IRuntimeUiValueProvider* provider)
{
    if (!provider) return false;
    const ModuleId moduleId = provider->runtimeUiProviderModuleId();
    if (!isValidModuleId(moduleId)) return false;

    const uint8_t index = moduleIdIndex(moduleId);
    if (providers_[index] != nullptr) return false;
    providers_[index] = provider;
    return true;
}

const IRuntimeUiValueProvider* RuntimeUiRegistry::providerForModule(ModuleId moduleId) const
{
    if (!isValidModuleId(moduleId)) return nullptr;
    return providers_[moduleIdIndex(moduleId)];
}

RuntimeUiBinaryWriter::RuntimeUiBinaryWriter(uint8_t* out, size_t capacity)
    : out_(out), capacity_(capacity)
{
}

bool RuntimeUiBinaryWriter::appendU8_(uint8_t value)
{
    if (overflowed_ || !out_ || length_ >= capacity_) {
        overflowed_ = true;
        return false;
    }
    out_[length_++] = value;
    return true;
}

bool RuntimeUiBinaryWriter::appendU16_(uint16_t value)
{
    return appendU8_((uint8_t)(value & 0xFFU)) &&
           appendU8_((uint8_t)((value >> 8) & 0xFFU));
}

bool RuntimeUiBinaryWriter::appendU32_(uint32_t value)
{
    return appendU8_((uint8_t)(value & 0xFFU)) &&
           appendU8_((uint8_t)((value >> 8) & 0xFFU)) &&
           appendU8_((uint8_t)((value >> 16) & 0xFFU)) &&
           appendU8_((uint8_t)((value >> 24) & 0xFFU));
}

bool RuntimeUiBinaryWriter::appendBytes_(const uint8_t* data, size_t len)
{
    if (overflowed_ || !out_ || (length_ + len) > capacity_) {
        overflowed_ = true;
        return false;
    }
    if (len > 0U && data) {
        memcpy(out_ + length_, data, len);
    }
    length_ += len;
    return true;
}

bool RuntimeUiBinaryWriter::writeHeader_(RuntimeUiId runtimeId, RuntimeUiWireType type, size_t payloadLen)
{
    if (overflowed_ || !out_ || (length_ + 3U + payloadLen) > capacity_) {
        overflowed_ = true;
        return false;
    }
    if (!appendU16_(runtimeId)) return false;
    if (!appendU8_((uint8_t)type)) return false;
    ++recordCount_;
    return true;
}

bool RuntimeUiBinaryWriter::writeBool(RuntimeUiId runtimeId, bool value)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::Bool, 1U) &&
           appendU8_(value ? 1U : 0U);
}

bool RuntimeUiBinaryWriter::writeI32(RuntimeUiId runtimeId, int32_t value)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::Int32, 4U) &&
           appendU32_((uint32_t)value);
}

bool RuntimeUiBinaryWriter::writeU32(RuntimeUiId runtimeId, uint32_t value)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::UInt32, 4U) &&
           appendU32_(value);
}

bool RuntimeUiBinaryWriter::writeF32(RuntimeUiId runtimeId, float value)
{
    uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value), "float/u32 size mismatch");
    memcpy(&bits, &value, sizeof(bits));
    return writeHeader_(runtimeId, RuntimeUiWireType::Float32, 4U) &&
           appendU32_(bits);
}

bool RuntimeUiBinaryWriter::writeEnum(RuntimeUiId runtimeId, uint8_t value)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::Enum, 1U) &&
           appendU8_(value);
}

bool RuntimeUiBinaryWriter::writeString(RuntimeUiId runtimeId, const char* value)
{
    const size_t len = value ? strnlen(value, 255U) : 0U;
    return writeHeader_(runtimeId, RuntimeUiWireType::String, 1U + len) &&
           appendU8_((uint8_t)len) &&
           appendBytes_(reinterpret_cast<const uint8_t*>(value), len);
}

bool RuntimeUiBinaryWriter::writeNotFound(RuntimeUiId runtimeId)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::NotFound, 0U);
}

bool RuntimeUiBinaryWriter::writeUnavailable(RuntimeUiId runtimeId)
{
    return writeHeader_(runtimeId, RuntimeUiWireType::Unavailable, 0U);
}

bool RuntimeUiService::readValues(const RuntimeUiId* ids,
                                  uint8_t count,
                                  uint8_t* out,
                                  size_t outCapacity,
                                  size_t& outLen) const
{
    outLen = 0U;
    if (!out || outCapacity == 0U) return false;

    out[0] = 0U;
    if (count == 0U) {
        outLen = 1U;
        return true;
    }

    RuntimeUiBinaryWriter writer(out + 1U, outCapacity - 1U);
    for (uint8_t i = 0; i < count; ++i) {
        const RuntimeUiId runtimeId = ids ? ids[i] : 0U;
        const uint8_t beforeCount = writer.recordCount();

        if (!isValidRuntimeUiId(runtimeId)) {
            if (!writer.writeNotFound(runtimeId)) return false;
            continue;
        }

        const ModuleId moduleId = (ModuleId)runtimeUiModuleId(runtimeId);
        const IRuntimeUiValueProvider* provider = registry_.providerForModule(moduleId);
        if (!provider) {
            if (!writer.writeNotFound(runtimeId)) return false;
            continue;
        }

        const bool ok = provider->writeRuntimeUiValue(runtimeUiValueId(runtimeId), writer);
        if ((!ok || writer.recordCount() == beforeCount) && !writer.writeNotFound(runtimeId)) {
            return false;
        }
        if (writer.overflowed()) return false;
    }

    out[0] = writer.recordCount();
    outLen = writer.length() + 1U;
    return !writer.overflowed();
}

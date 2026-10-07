#pragma once
#include "ConfigTypes.h"
#include "ConfigDoubleAccess.h"
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <type_traits>

/** Read-only overlay used by module validators before any RAM/NVS mutation.
 * JSON values for validated fields must use their declared JSON type. Native
 * setters supply one typed replacement without serializing the configuration. */
class ConfigCandidate {
public:
    ConfigCandidate() = default;
    explicit ConfigCandidate(JsonObjectConst patch) : patch_(patch) {}
    ConfigCandidate(const void* target, const void* replacement)
        : target_(target), replacement_(replacement) {}

    template<typename T, size_t H>
    bool contains(const ConfigVariable<T,H>& var) const {
        return (target_ && target_ == var.value) ||
            (var.moduleName && var.jsonName && patch_[var.moduleName].containsKey(var.jsonName));
    }
    template<typename T, size_t H>
    bool read(const ConfigVariable<T,H>& var, T current, T& out) const {
        out = current;
        if (target_ && target_ == var.value) out = *static_cast<const T*>(replacement_);
        else if (contains(var)) {
            const auto value = patch_[var.moduleName][var.jsonName];
            if (!value.template is<T>()) return false;
            out = value.template as<T>();
        }
        if constexpr (std::is_floating_point<T>::value) return std::isfinite(out);
        return true;
    }
    template<size_t H, size_t N>
    bool read(const ConfigVariable<char,H>& var, const char* current, char (&out)[N]) const {
        const char* text = current;
        if (target_ && target_ == var.value) text = static_cast<const char*>(replacement_);
        else if (contains(var)) {
            const auto value = patch_[var.moduleName][var.jsonName];
            if (!value.template is<const char*>()) return false;
            text = value.template as<const char*>();
        }
        if (!text || strlen(text) >= N) return false;
        memcpy(out, text, strlen(text) + 1);
        return true;
    }
private:
    JsonObjectConst patch_;
    const void* target_ = nullptr;
    const void* replacement_ = nullptr;
};

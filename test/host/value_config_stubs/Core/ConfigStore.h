#pragma once
#include <stdint.h>
#include <vector>
#include <string>

enum class ConfigType { Bool, UInt16, Double, UInt8, CharArray, Int32, Float };
enum class ConfigPersistence { Persistent };
template<typename T, int> struct ConfigVariable {
    const char* key{};
    const char* name{};
    const char* path{};
    ConfigType type{};
    T* value{};
    ConfigPersistence persistence{};
    int flags{};
    bool (*validateText)(const char*) = nullptr;
};
struct ConfigStore {
    struct Entry { std::string key, name, path; ConfigType type; uint8_t branch; };
    std::vector<Entry> entries;
    template<typename T> void registerVar(ConfigVariable<T, 0>& v, uint8_t, uint8_t branch) {
        entries.push_back({v.key, v.name, v.path, v.type, branch});
    }
};

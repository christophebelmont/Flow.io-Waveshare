#pragma once
#include <stdint.h>
#include <vector>
#include <string>

#include "Core/ConfigTypes.h"
struct ConfigStore {
    struct Entry { std::string key, name, path; ConfigType type; uint8_t branch; };
    std::vector<Entry> entries;
    template<typename T> void registerVar(ConfigVariable<T, 0>& v, uint8_t, uint8_t branch) {
        entries.push_back({v.nvsKey, v.jsonName, v.moduleName, v.type, branch});
    }
};

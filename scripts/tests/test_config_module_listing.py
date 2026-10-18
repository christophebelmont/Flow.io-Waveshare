"""Exercise actual ConfigStore enumeration beyond the former 160/255 limits."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class ConfigModuleListingTests(unittest.TestCase):
    def test_full_store_and_duplicates(self):
        source = (ROOT / 'src/Core/ConfigStore.cpp').read_text()
        start = source.index('size_t ConfigStore::listModules(')
        end = source.index('\nbool ConfigStore::applyJson', start)
        production = source[start:end]
        limits = (ROOT / 'include/Core/SystemLimits.h').read_text()
        capacity = int(re.search(r'MaxConfigVars = (\d+)', limits).group(1))
        self.assertIn('ModuleListMax = MaxConfigVars', limits)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
struct ConfigMeta { const char* module; };
struct ConfigStore {
    std::vector<ConfigMeta> _meta;
    uint16_t _metaCount = 0;
    size_t listModules(const char** out, size_t max) const;
};
// PRODUCTION
int main() {
    constexpr size_t capacity = CAPACITY;
    std::vector<std::string> names; names.reserve(capacity);
    for (size_t i=0; i<capacity; ++i) names.push_back("config/branch/" + std::to_string(i));
    ConfigStore store;
    for (const auto& name : names) store._meta.push_back({name.c_str()});
    store._metaCount = store._meta.size();
    std::vector<const char*> out(capacity + 1, nullptr);
    assert(store.listModules(out.data(), capacity) == capacity);
    for (size_t i=0; i<capacity; ++i) assert(strcmp(out[i], names[i].c_str())==0);
    assert(out[capacity] == nullptr);
    assert(store.listModules(nullptr, capacity) == 0);
    assert(store.listModules(out.data(), 0) == 0);
    assert(store.listModules(out.data(), 7) == 7);
    // 330 capability variables registered first, then all pre-existing branches.
    store._meta.clear();
    for (size_t i=0; i<110; ++i) for (size_t field=0; field<3; ++field)
        store._meta.push_back({names[i].c_str()});
    for (size_t i=110; i<500; ++i) store._meta.push_back({names[i].c_str()});
    store._meta.push_back({nullptr}); store._meta.push_back({""});
    store._metaCount = store._meta.size();
    assert(store.listModules(out.data(), capacity)==500);
    for (size_t i=0; i<500; ++i) assert(strcmp(out[i], names[i].c_str())==0);
    puts("ConfigStore: full capacity, >255 branches, capability duplicates and late modules preserved OK");
}
'''.replace('// PRODUCTION', production).replace('CAPACITY', str(capacity))
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / 'listing.cpp'; exe = Path(tmp) / 'listing'
            cpp.write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__': unittest.main()

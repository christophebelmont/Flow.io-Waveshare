"""Growing PSRAM metadata preserves registrations and initializes persistence flags."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from scripts.tests.test_io_discovery_boot import function
ROOT = Path(__file__).resolve().parents[2]

class SparseConfigStorageTests(unittest.TestCase):
    def test_growth_and_failure(self):
        production = function((ROOT/'src/Core/ConfigStore.cpp').read_text(), 'bool ConfigStore::ensureMetaStorage_(')
        harness = r'''
#include "Core/ConfigTypes.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
bool fail=false;
size_t previousBytes=0;
void* heap_caps_realloc(void* memory, size_t bytes, unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(fail) return nullptr;
    auto* result=std::realloc(memory,bytes);
    assert(result);
    memset(static_cast<char*>(result)+previousBytes,0xa5,bytes-previousBytes);
    previousBytes=bytes;
    return result;
}
struct ConfigStore {
    static constexpr size_t MAX_CONFIG_VARS=3072;
    ConfigMeta* _meta=nullptr;
    uint16_t _metaCount=0;
    size_t _metaCapacity=0;
    bool ensureMetaStorage_(size_t required=0);
    ~ConfigStore(){std::free(_meta);}
};
'''
        scenario = r'''
int main(){
    ConfigStore store;
    assert(store.ensureMetaStorage_());
    assert(store._metaCapacity==64);
    int value=7;
    store._meta[0].valuePtr=&value;
    store._meta[0].persistencePending=true;
    store._metaCount=1;
    assert(store.ensureMetaStorage_(257));
    assert(store._metaCapacity==320);
    assert(store._meta[0].valuePtr==&value && store._meta[0].persistencePending);
    for(size_t i=1;i<320;++i) {
        assert(!store._meta[i].persistencePending);
        assert(store._meta[i].moduleId==0 && store._meta[i].localBranchId==0);
    }
    fail=true;
    assert(!store.ensureMetaStorage_(321));
    assert(store._metaCapacity==320 && store._meta[0].valuePtr==&value);
    fail=false;
    assert(!store.ensureMetaStorage_(3073));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'test.cpp'; exe=Path(tmp)/'test'
            cpp.write_text(harness+production+scenario)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-Isrc','-Iinclude',str(cpp),'-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()

"""Exercise the production refill boundary: litres requested, mL capacity configured."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
class TankLitresTests(unittest.TestCase):
    def test_refill_units(self):
        source = (ROOT/'src/Modules/PoolDeviceModule/PoolDeviceControl.cpp').read_text()
        a = source.index('PoolDeviceSvcStatus PoolDeviceModule::svcRefillTankImpl_')
        b = source.index('\nvoid PoolDeviceModule::', a)
        code = r'''
#include "Core/Values/VolumeUnits.h"
#include <cstdint>
#include <cassert>
constexpr uint8_t POOL_DEVICE_MAX = 1;
enum PoolDeviceSvcStatus {POOLDEV_SVC_OK, POOLDEV_SVC_ERR_NOT_READY, POOLDEV_SVC_ERR_UNKNOWN_SLOT};
uint32_t millis() { return 1; }
struct PoolDeviceModule {
 struct PoolDeviceSlot {
  bool used=true, forceMetricsCommit=false, persistDirty=false, persistImmediate=false;
  struct {float tankCapacityMl=10000;} def;
  float tankRemainingL=0;
 } slots_[1];
 bool runtimeReady_=false;
 bool lockState_() {return true;}
 void unlockState_() {}
 void tickDevices_(uint32_t,bool) {}
 PoolDeviceSvcStatus svcRefillTankImpl_(uint8_t,float);
};
// PRODUCTION
int main() {
 PoolDeviceModule module;
 assert(module.svcRefillTankImpl_(0, 2.5f)==POOLDEV_SVC_OK);
 assert(module.slots_[0].tankRemainingL==2.5f);
 assert(module.slots_[0].persistDirty && module.slots_[0].persistImmediate);
 assert(module.svcRefillTankImpl_(0, 20)==POOLDEV_SVC_OK);
 assert(module.slots_[0].tankRemainingL==10);
 assert(module.svcRefillTankImpl_(0, -1)==POOLDEV_SVC_OK);
 assert(module.slots_[0].tankRemainingL==0);
 assert(module.svcRefillTankImpl_(1, 5)==POOLDEV_SVC_ERR_UNKNOWN_SLOT);
}
'''.replace('// PRODUCTION', source[a:b])
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'tank.cpp'; exe=Path(tmp)/'tank';cpp.write_text(code)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-Isrc',str(cpp),'-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()

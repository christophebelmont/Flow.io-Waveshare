"""Execute the production release reboot gate with checkpoint and boot-selection failures."""
import pathlib
import subprocess
import tempfile
import unittest
from test_io_discovery_boot import function

ROOT = pathlib.Path(__file__).resolve().parents[2]

class ReleaseCheckpointTests(unittest.TestCase):
    def test_reboot_gate(self):
        source = (ROOT / 'src/Modules/Network/FirmwareUpdateModule/FirmwareUpdateModule.cpp').read_text()
        production = function(source, 'bool FirmwareUpdateModule::prepareLocalReleaseReboot_()')
        harness = r'''
#include <cassert>
#include <cstring>
constexpr int ESP_OK = 0;
enum class FirmwareUpdateTarget { Waveshare };
enum class FirmwareUpdateReceiptState { Failed };
struct SemaphoreGuard { explicit SemaphoreGuard(int) {} bool acquired() const { return true; } };
int running = 1, selected = 2, restoreCalls = 0;
bool restoreWorks = true;
const int* esp_ota_get_running_partition() { return &running; }
int esp_ota_set_boot_partition(const int* partition) {
    ++restoreCalls;
    if (!restoreWorks) return 1;
    selected = *partition; return ESP_OK;
}
struct FirmwareUpdateModule {
    bool checkpointWorks = true;
    unsigned checkpoints = 0, failedReceipts = 0;
    int localReleaseMutex_ = 0;
    struct { bool rebootPending = true; unsigned operationId = 12; } localRelease_;
    const char* failure = nullptr;
    bool saveCountersBeforeUpdate_(char*, unsigned long) { ++checkpoints; return checkpointWorks; }
    bool persistReceipt_(FirmwareUpdateTarget, unsigned id, FirmwareUpdateReceiptState) {
        assert(id == 12); ++failedReceipts; return true;
    }
    void failLocalRelease_(const char* reason) { failure = reason; }
    bool prepareLocalReleaseReboot_();
};
'''
        scenarios = r'''
int main() {
    FirmwareUpdateModule success;
    assert(success.prepareLocalReleaseReboot_());
    assert(success.checkpoints == 1 && success.localRelease_.rebootPending);
    assert(!success.failure && !success.failedReceipts && !restoreCalls && selected == 2);
    FirmwareUpdateModule cancelled;
    cancelled.localRelease_.rebootPending = false;
    assert(!cancelled.prepareLocalReleaseReboot_() && cancelled.checkpoints == 0);
    FirmwareUpdateModule failed;
    failed.checkpointWorks = false;
    assert(!failed.prepareLocalReleaseReboot_());
    assert(!failed.localRelease_.rebootPending && failed.failedReceipts == 1);
    assert(strstr(failed.failure, "reboot cancelled") && selected == running && restoreCalls == 1);
    FirmwareUpdateModule doubleFailure;
    doubleFailure.checkpointWorks = restoreWorks = false;
    assert(!doubleFailure.prepareLocalReleaseReboot_());
    assert(!doubleFailure.localRelease_.rebootPending && doubleFailure.failedReceipts == 1);
    assert(strstr(doubleFailure.failure, "boot partition restore failed"));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / 'checkpoint.cpp'
            exe = pathlib.Path(tmp) / 'checkpoint'
            cpp.write_text(harness + production + scenarios)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()

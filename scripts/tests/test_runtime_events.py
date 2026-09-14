"""Check production notification batching without an ESP32 runtime."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROGRAM = r'''
#include "Modules/Network/WebInterfaceModule/RuntimeEvents.h"
#include <cassert>

int main() {
    RuntimeEventState state;
    RuntimeEventBatch batch{};
    assert(state.revision() == 1);
    assert(!state.take(100, batch));
    state.mark(RuntimeEventDomains::Equipment);
    state.mark(RuntimeEventDomains::Alarm);
    state.mark(RuntimeEventDomains::Equipment);
    assert(!state.take(99, batch));
    assert(state.take(100, batch));
    assert(batch.domains == (RuntimeEventDomains::Equipment | RuntimeEventDomains::Alarm));
    assert(batch.revision == 2);
    assert(!state.take(200, batch));
    assert(state.revision() == 2);

    // A notification during transmission belongs to the next batch.
    state.mark(RuntimeEventDomains::Mode);
    assert(!state.take(199, batch));
    assert(state.take(200, batch));
    assert(batch.domains == RuntimeEventDomains::Mode && batch.revision == 3);
    state.mark(0xF0); // Unknown wire flags must not create notifications.
    assert(!state.take(300, batch));
    state.mark(RuntimeEventDomains::All);
    assert(state.take(UINT32_MAX - 50U, batch));
    state.mark(RuntimeEventDomains::Alarm);
    assert(!state.take(20, batch));
    assert(state.take(49, batch)); // millis() wraps without losing the interval.
    assert(batch.domains == RuntimeEventDomains::Alarm && batch.revision == 5);
}
'''

class RuntimeEventsTest(unittest.TestCase):
    def test_batching_preserves_changes_and_handles_clock_wrap(self):
        self.assertIsNotNone(shutil.which("c++"), "A native C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="flow-runtime-events-") as directory:
            source = Path(directory) / "main.cpp"
            executable = Path(directory) / "test"
            source.write_text(PROGRAM)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "src"), str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    unittest.main()

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Core/Log.h"

uint32_t testNowMs = 42;
namespace {
bool enabled = true;
std::vector<LogEntry> entries;
bool enqueue(void*, const LogEntry& entry) { entries.push_back(entry); return true; }
bool shouldLog(void*, LogModuleId module, LogLevel level) {
    assert(module == (LogModuleId)LogModuleIdValue::IOModule);
    assert(level == LogLevel::Debug);
    return enabled;
}
void truncated(void*, LogModuleId, uint32_t) { assert(false); }
}

int main() {
    LogHubService hub{};
    hub.enqueue = enqueue;
    hub.shouldLog = shouldLog;
    hub.noteFormatTruncation = truncated;
    const auto module = (LogModuleId)LogModuleIdValue::IOModule;
    uint8_t data[256];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    Log::debugHex(module, "Modbus RX", 65535, data, sizeof(data));
    assert(entries.empty()); // No hub.
    Log::setHub(&hub);
    enabled = false;
    Log::debugHex(module, "Modbus RX", 65535, data, sizeof(data));
    assert(entries.empty());
    enabled = true;
    Log::debugHex(module, "Modbus RX", 65535, data, sizeof(data));
    size_t recovered = 0;
    for (const auto& entry : entries) {
        assert(entry.lvl == LogLevel::Debug && entry.moduleId == module && entry.ts_ms == 42);
        unsigned id, offset, count, length;
        int hexStart = 0;
        assert(std::sscanf(entry.msg, "Modbus RX id=%u offset=%u bytes=%u/%u hex=%n",
                           &id, &offset, &count, &length, &hexStart) == 4);
        assert(id == 65535 && offset == recovered && count > 0 && count <= 24 && length == sizeof(data));
        assert(std::strlen(entry.msg + hexStart) == count * 3 - 1);
        for (unsigned i = 0; i < count; ++i) {
            unsigned byte;
            assert(std::sscanf(entry.msg + hexStart + i * 3, "%2x", &byte) == 1);
            assert(byte == data[recovered++]);
        }
    }
    assert(recovered == sizeof(data));
    entries.clear();
    Log::debugHex(module, "Modbus TX", 1, data, 1);
    assert(entries.size() == 1);
    assert(std::string(entries[0].msg) == "Modbus TX id=1 offset=0 bytes=1/1 hex=00");
    entries.clear();
    Log::debugHex(module, "Modbus TX", 1, data, 0);
    Log::debugHex(module, "Modbus TX", 1, nullptr, 10);
    assert(entries.empty());
    Log::setHub(nullptr);
}

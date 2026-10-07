#pragma once

#ifndef FLOW_MEMORY_DIAGNOSTICS
#define FLOW_MEMORY_DIAGNOSTICS 0
#endif

class AsyncWebServer;

namespace WebMemoryDiagnostics {
#if FLOW_MEMORY_DIAGNOSTICS
void install(AsyncWebServer& server);
void sample(const char* phase);
void poll();
#else
inline void install(AsyncWebServer&) {}
inline void sample(const char*) {}
inline void poll() {}
#endif
} // namespace WebMemoryDiagnostics

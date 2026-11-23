#pragma once

// Optional runtime profiling. Enable explicitly in the compiler build flags.
#ifndef FLOW_PERFORMANCE_DIAGNOSTICS
#define FLOW_PERFORMANCE_DIAGNOSTICS 0
#endif

#if FLOW_PERFORMANCE_DIAGNOSTICS != 0 && FLOW_PERFORMANCE_DIAGNOSTICS != 1
#error "FLOW_PERFORMANCE_DIAGNOSTICS must be 0 or 1"
#endif

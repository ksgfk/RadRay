#pragma once

// Instrumentation entry point for the whole codebase. Business code uses only these macros and never
// includes the profiler backend directly, so the backend can be swapped or compiled out per configuration.
// Design and usage notes: docs/architecture/core-facilities.md
//
// Contract:
// - RADRAY_PROFILE_SCOPE()          zone named after the enclosing function.
// - RADRAY_PROFILE_SCOPE_N("lit")   zone with a string-literal name (must have static storage).
// - RADRAY_PROFILE_SCOPE_DYN(sv)    zone whose name is a runtime std::string_view (copied by the profiler).
// - RADRAY_PROFILE_FRAME()          marks the end of one frame on the calling thread.
// - RADRAY_PROFILE_PLOT("name", v)  plots a numeric value under a string-literal series name.
// - RADRAY_PROFILE_THREAD("name")   names the calling thread (string literal).
// - RADRAY_PROFILE_MESSAGE(sv)      appends a text message to the timeline.
// All macros expand to nothing when RADRAY_ENABLE_PROFILER is undefined. Zones are cheap (tens of ns)
// but not free; keep them at the granularity of frame stages, passes and per-draw batches, not per element.

#include <string_view>

#ifdef RADRAY_ENABLE_PROFILER

#include <tracy/Tracy.hpp>

#define RADRAY_PROFILE_SCOPE() ZoneScoped
#define RADRAY_PROFILE_SCOPE_N(name) ZoneScopedN(name)
#define RADRAY_PROFILE_SCOPE_DYN(name)                                                     \
    ZoneScoped;                                                                            \
    do {                                                                                   \
        const std::string_view radray_profile_dyn_name_{name};                             \
        ZoneName(radray_profile_dyn_name_.data(), radray_profile_dyn_name_.size());        \
    } while (false)
#define RADRAY_PROFILE_FRAME() FrameMark
#define RADRAY_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define RADRAY_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define RADRAY_PROFILE_MESSAGE(text)                                          \
    do {                                                                      \
        const std::string_view radray_profile_msg_{text};                     \
        TracyMessage(radray_profile_msg_.data(), radray_profile_msg_.size()); \
    } while (false)

#else

#define RADRAY_PROFILE_SCOPE()
#define RADRAY_PROFILE_SCOPE_N(name)
#define RADRAY_PROFILE_SCOPE_DYN(name) ((void)0)
#define RADRAY_PROFILE_FRAME()
#define RADRAY_PROFILE_PLOT(name, value) ((void)0)
#define RADRAY_PROFILE_THREAD(name) ((void)0)
#define RADRAY_PROFILE_MESSAGE(text) ((void)0)

#endif

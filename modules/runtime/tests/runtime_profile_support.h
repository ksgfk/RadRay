#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <filesystem>
#include <fstream>
#include <thread>

#include <fmt/format.h>
#include <radray/types.h>
#include <radray/profiler.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>

#include "runtime_profile_build_identity.h"

namespace radray::profile {

inline uint32_t EnvironmentCount(const char* name, uint32_t fallback) noexcept {
    const auto* value = std::getenv(name);
    if (!value) return fallback;
    const std::string_view input{value};
    uint32_t parsed = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size() && parsed > 0 ? parsed : 0;
}

struct Options {
    bool Extended{std::getenv("RADRAY_RUNTIME_PROFILE") != nullptr};
    uint32_t Warmup{EnvironmentCount("RADRAY_PROFILE_WARMUP", Extended ? 120u : 3u)};
    uint32_t Samples{EnvironmentCount("RADRAY_PROFILE_SAMPLES", Extended ? 1000u : 3u)};
    uint32_t Rounds{EnvironmentCount("RADRAY_PROFILE_ROUNDS", Extended ? 5u : 1u)};
    uint32_t Primitives{EnvironmentCount("RADRAY_PROFILE_PRIMITIVES", 1000u)};
    RenderGraphRuntimeOptions Runtime{kPerformanceRenderGraphRuntimeOptions};
    bool SerializeReport{false};
    bool DriverValidation{false};
    bool Valid{true};

    Options() {
        if (const auto* mode = std::getenv("RADRAY_PROFILE_VALIDATION")) {
            if (std::string_view{mode} == "full") Runtime.Validation = RenderValidationMode::Full;
            else if (std::string_view{mode} != "off") Valid = false;
        }
        if (const auto* mode = std::getenv("RADRAY_PROFILE_REPORT")) {
            if (std::string_view{mode} == "full") Runtime.Report = RenderGraphReportMode::Full;
            else if (std::string_view{mode} == "counters") Runtime.Report = RenderGraphReportMode::Counters;
            else if (std::string_view{mode} != "minimal") Valid = false;
        }
        Runtime.GpuMarkers = std::getenv("RADRAY_PROFILE_GPU_MARKERS") != nullptr;
        SerializeReport = std::getenv("RADRAY_PROFILE_SERIALIZE") != nullptr;
        DriverValidation = std::getenv("RADRAY_PROFILE_DRIVER_VALIDATION") != nullptr;
        const uint64_t totalFrames = (uint64_t{Warmup} + Samples) * Rounds;
        Valid = Valid && Warmup && Samples && Rounds && Primitives && totalFrames <= UINT32_MAX - 8u && (!SerializeReport || Runtime.Report == RenderGraphReportMode::Full);
    }
};

#if defined(RADRAY_ENABLE_PROFILER)
inline constexpr bool ProfilerEnabled = true;
#else
inline constexpr bool ProfilerEnabled = false;
#endif
#if defined(RADRAY_RUNTIME_DETAILED_PROFILING)
inline constexpr bool DetailedProfilingEnabled = true;
#else
inline constexpr bool DetailedProfilingEnabled = false;
#endif

inline void PrintBuildIdentity() {
    // fmt debug-string presentation quotes and escapes the compiler/flags for JSON.
    fmt::print("PROFILE_BUILD {{\"commit\":{:?},\"trackedDirty\":{},\"trackedDiffSha256\":{:?},\"runtimeTestsSha256\":{:?},\"runtimeSourcesSha256\":{:?},\"harnessSha256\":{:?},\"compiler\":{:?},\"flags\":{:?},\"configuration\":{:?},\"profilerEnabled\":{},\"detailedProfilingEnabled\":{}}}\n",
               std::string_view{BuildCommit}, BuildTrackedDirty, std::string_view{BuildDiffHash}, std::string_view{BuildTestsHash}, std::string_view{BuildRuntimeHash}, std::string_view{BuildHarnessHash}, std::string_view{BuildCompiler}, std::string_view{BuildFlags}, std::string_view{BuildConfiguration}, ProfilerEnabled, DetailedProfilingEnabled);
}

inline uint64_t TimestampNs() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::string_view RunIdentity() noexcept {
    const auto* value = std::getenv("RADRAY_PROFILE_RUN_ID");
    return value ? std::string_view{value} : std::string_view{"direct"};
}

// The controller releases this startup-only permit after matching the capture/client TCP pair.
// It precedes Application startup, warmup and every measured frame; no submission policy changes.
inline bool AwaitCapturePermit() {
    const auto* gate = std::getenv("RADRAY_PROFILE_CAPTURE_GATE");
    if (!gate) return true;
    if (!ProfilerEnabled || RunIdentity() == "direct") return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{40};
    const std::filesystem::path path{gate};
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            std::ifstream stream{path};
            string permit;
            std::getline(stream, permit);
            if (permit == RunIdentity()) return true;
        }
        if (error && error != std::errc::no_such_file_or_directory) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

inline void TraceMarker(uint32_t index, uint32_t flight, uint64_t serial, std::string_view stage) {
#if defined(RADRAY_ENABLE_PROFILER)
    array<char, 384> storage;
    const auto text = fmt::format_to_n(storage.data(), storage.size(), "RRP2|index={}|flight={}|serial={}|stage={}|run={}|build={}|harness={}",
                                       index, flight, serial, stage, RunIdentity(), std::string_view{BuildRuntimeHash}, std::string_view{BuildHarnessHash});
    const std::string_view message{storage.data(), std::min(text.size, storage.size())};
    // csvexport messages omit thread identity. The enclosing zone supplies its exact
    // exported thread index without assuming it is an operating-system thread ID.
    RADRAY_PROFILE_SCOPE_DYN(message);
    RADRAY_PROFILE_MESSAGE(message);
#else
    (void)index; (void)flight; (void)serial; (void)stage;
#endif
}

inline double QuantileMs(vector<uint64_t> values, uint32_t percentile) {
    std::sort(values.begin(), values.end());
    return double(values[std::min(values.size() - 1, (values.size() * percentile + 99) / 100 - 1)]) / 1e6;
}

inline double CoefficientOfVariation(std::span<const double> values) noexcept {
    double sum = 0;
    for (const auto value : values) sum += value;
    const auto mean = sum / double(values.size());
    if (mean == 0) return 0;
    double variance = 0;
    for (const auto value : values) variance += (value - mean) * (value - mean);
    return std::sqrt(variance / double(values.size())) / mean;
}

inline void PrintOptions(const Options& options, std::string_view fixture, std::string_view backend, uint32_t views, uint32_t flights, uint32_t width, uint32_t height, bool multithreaded, std::string_view snapshotMode = "micro") {
    PrintBuildIdentity();
    fmt::print("PROFILE_RUN {{\"id\":{:?}}}\n", RunIdentity());
    fmt::print("PROFILE_INSTRUMENTATION {{\"detailedProfiling\":{},\"frameAndPhaseProfiling\":{},\"cpuSampling\":\"external evidence required\"}}\n", DetailedProfilingEnabled, ProfilerEnabled);
    fmt::print("PROFILE_PHASE_CONTRACT {{\"traceSchema\":2,\"snapshotMode\":{:?},\"gtGuard\":\"RenderSystem::PrepareFrame\",\"workerCoverage\":\"unknown\",\"authoringScope\":\"fixture rendering-proxy setter batch\",\"clock\":\"Tracy inclusive zone/message clock\"}}\n", snapshotMode);
    fmt::print("PROFILE_ASSETS {{\"sourceRoot\":{:?},\"geometry\":\"procedural-runtime-profile-v2\",\"texture\":\"procedural-1x1-white-v1\",\"shaderDirectory\":\"shaderlib\"}}\n", std::string_view{RADRAY_PROJECT_DIR});
    fmt::print("PROFILE_OPTIONS {{\"fixture\":{:?},\"fixtureVersion\":2,\"backend\":{:?},\"warmup\":{},\"samples\":{},\"rounds\":{},\"primitives\":{},\"rgValidation\":{:?},\"rgReport\":{:?},\"gpuMarkers\":{},\"serializeReport\":{},\"driverValidation\":{},\"views\":{},\"flights\":{},\"width\":{},\"height\":{},\"multithreaded\":{},\"vsync\":false,\"scopeMode\":\"inclusive\",\"quantile\":\"nearest-rank\"}}\n",
               fixture, backend, options.Warmup, options.Samples, options.Rounds, options.Primitives,
               options.Runtime.Validation == RenderValidationMode::Full ? "full" : "off",
               options.Runtime.Report == RenderGraphReportMode::Full ? "full" : options.Runtime.Report == RenderGraphReportMode::Counters ? "counters" : "minimal",
               options.Runtime.GpuMarkers, options.SerializeReport, options.DriverValidation, views, flights, width, height, multithreaded);
}

}  // namespace radray::profile

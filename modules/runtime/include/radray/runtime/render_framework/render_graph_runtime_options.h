#pragma once

#include <cstdint>

namespace radray {

enum class RenderValidationMode : uint8_t {
    Off,
    Full,
};

enum class RenderGraphReportMode : uint8_t {
    Minimal,
    Counters,
    Full,
};

/// Per-flight graph/draw runtime policy. Frozen before PrepareFrame; not a global service.
struct RenderGraphRuntimeOptions {
    RenderValidationMode Validation{RenderValidationMode::Off};
    RenderGraphReportMode Report{RenderGraphReportMode::Minimal};
    bool GpuMarkers{false};
    friend bool operator==(const RenderGraphRuntimeOptions&, const RenderGraphRuntimeOptions&) = default;
};

/// Application / Tidal / benchmarks: skip developer-contract checks.
inline constexpr RenderGraphRuntimeOptions kPerformanceRenderGraphRuntimeOptions{};

/// Isolated graph tests and explicit diagnostics: current pre-gating behavior.
inline constexpr RenderGraphRuntimeOptions kDiagnosticRenderGraphRuntimeOptions{
    RenderValidationMode::Full,
    RenderGraphReportMode::Full,
    true};

inline constexpr bool IsRenderValidationFull(RenderValidationMode mode) noexcept {
    return mode == RenderValidationMode::Full;
}

inline constexpr bool IsRenderGraphReportFull(RenderGraphReportMode mode) noexcept {
    return mode == RenderGraphReportMode::Full;
}

inline constexpr bool IsRenderGraphReportMinimal(RenderGraphReportMode mode) noexcept {
    return mode == RenderGraphReportMode::Minimal;
}

}  // namespace radray

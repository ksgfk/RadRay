#pragma once

#include <cstdint>

namespace radray {

/// Lower values render first; values at or above GeometryLast use transparent sorting.
enum class RenderQueue : int32_t {
    Background = 1000,
    Geometry = 2000,
    AlphaTest = 2450,
    GeometryLast = 2500,
    Transparent = 3000,
    Overlay = 4000,
};

struct BindingGroupPlan {
    constexpr BindingGroupPlan(
        uint32_t viewGroup,
        uint32_t materialGroup,
        uint32_t objectGroup) noexcept
        : ViewGroup(viewGroup),
          MaterialGroup(materialGroup),
          ObjectGroup(objectGroup) {}

    constexpr bool IsValid() const noexcept {
        return ViewGroup != MaterialGroup && ViewGroup != ObjectGroup &&
               MaterialGroup != ObjectGroup;
    }

    uint32_t ViewGroup;
    uint32_t MaterialGroup;
    uint32_t ObjectGroup;

    friend bool operator==(const BindingGroupPlan&, const BindingGroupPlan&) = default;
};

}  // namespace radray

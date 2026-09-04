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

}  // namespace radray

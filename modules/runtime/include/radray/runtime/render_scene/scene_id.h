#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace radray {

struct SceneId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const SceneId&) const noexcept = default;
};

/// Local to a SceneId. Never use this identity alone across scenes.
struct ShapeId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const ShapeId&) const noexcept = default;
};

/// Local to a SceneId, with a separate identity pool from shapes.
struct LightId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const LightId&) const noexcept = default;
};

}  // namespace radray

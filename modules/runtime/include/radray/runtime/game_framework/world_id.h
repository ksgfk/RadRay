#pragma once

#include <cstdint>
#include <limits>
#include <compare>

namespace radray {

struct WorldId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const WorldId&) const noexcept = default;
};

}  // namespace radray

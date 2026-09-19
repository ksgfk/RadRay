#pragma once

#include <radray/sparse_set.h>

namespace radray {

struct SceneId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const SceneId&) const noexcept = default;
};

/// Local to a SceneId. Never use this identity alone across scenes.
using PrimitiveId = SparseSetHandle;

}  // namespace radray

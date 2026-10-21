#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <radray/guid.h>

namespace radray {
/// Local to one sealed packet. Consumers resolve it while reading, then retain their own representation.
template <class Tag> struct ScenePacketId {
    Guid Packet;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const ScenePacketId&, const ScenePacketId&) = default;
};
struct SceneMaterialListTag;
struct SceneInstanceSchemaTag;
using SceneMaterialListId = ScenePacketId<SceneMaterialListTag>;
using InstanceSchemaId = ScenePacketId<SceneInstanceSchemaTag>;

/// A local transform in one SceneId; independent of shape and light identities.
struct TransformId {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const TransformId&) const noexcept = default;
};

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

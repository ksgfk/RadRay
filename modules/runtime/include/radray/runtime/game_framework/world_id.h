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

/// IDs are scoped to their World (and its WorldManager); they do not keep objects alive.
struct ActorId {
    WorldId World;
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const ActorId&) const noexcept = default;
};

struct ComponentId {
    ActorId Actor;
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    constexpr auto operator<=>(const ComponentId&) const noexcept = default;
};

enum class ObjectLifecycle : uint8_t { Initializing,
                                       Live,
                                       PendingDestroy,
                                       Destroying };
enum class ComponentRegistration : uint8_t { Unregistered,
                                             Registering,
                                             Registered,
                                             Unregistering };
enum class LifecycleRequestResult : uint8_t { Accepted,
                                              AlreadyPending,
                                              Invalid };
enum class AttachmentRule : uint8_t { KeepLocal,
                                      KeepWorld };
enum class RenderConnectionState : uint8_t { Disconnected,
                                             Connecting,
                                             Connected,
                                             Disconnecting };

}  // namespace radray

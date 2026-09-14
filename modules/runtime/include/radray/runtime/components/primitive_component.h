#pragma once

#include <radray/runtime/components/scene_component.h>

namespace radray {

/// Base for components carrying drawable data; owns no renderer state.
class PrimitiveComponent : public SceneComponent {
public:
    PrimitiveComponent() noexcept = default;
    ~PrimitiveComponent() noexcept override;
};

template <>
struct RuntimeTypeTrait<PrimitiveComponent> {
    static constexpr RuntimeTypeId value{0xfb11f0d6, 0xc97b, 0x4f3f, 0x98, 0xe3, 0xf5, 0x16, 0x8c, 0xbf, 0x0f, 0x42};
};

}  // namespace radray

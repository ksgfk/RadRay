#pragma once

#include <radray/types.h>

namespace radray {
enum class MaterialInputSource : uint8_t { SceneObjects,
                                           ViewConstants,
                                           MaterialConstants,
                                           Texture,
                                           Sampler,
                                           FrameConstants,
                                           InstanceParameters,
                                           VisibleInstanceSlots };
enum class MaterialInstanceAddressing : uint8_t { DirectObject, VisibleSlots };
struct MaterialInput {
    string Name;
    MaterialInputSource Source;
    uint32_t ValueIndex{0};
    friend bool operator==(const MaterialInput&, const MaterialInput&) = default;
};
enum class MaterialBindingDomain : uint8_t { SceneFrame, ViewPass, Material, Sampler };
}  // namespace radray

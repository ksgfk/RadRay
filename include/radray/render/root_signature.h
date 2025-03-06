#pragma once

#include <radray/render/common.h>

namespace radray::render {

struct RootConstantInfo {
    uint32_t Slot;
    uint32_t Space;
    uint32_t Size;
    ShaderStages Stages;
};

struct RootDescriptorInfo {
    uint32_t Slot;
    uint32_t Space;
    ResourceType Type;
    ShaderStages Stages;
};

struct DescriptorSetElementInfo {
    uint32_t Slot;
    uint32_t Space;
    uint32_t Count;
    ResourceType Type;
    ShaderStages Stages;
};

class DescriptorSetInfo {
public:
    radray::vector<DescriptorSetElementInfo> Elements;
};

struct RootSignatureDescriptor {
    std::span<RootConstantInfo> RootConstants;
    std::span<RootDescriptorInfo> RootDescriptors;
    std::span<DescriptorSetInfo> DescriptorSets;
};

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;

    virtual std::span<const RootConstantInfo> GetRootConstants() const noexcept = 0;

    virtual std::span<const RootDescriptorInfo> GetRootDescriptors() const noexcept = 0;

    virtual std::span<const DescriptorSetElementInfo> GetBindDescriptors() const noexcept = 0;
};

}  // namespace radray::render

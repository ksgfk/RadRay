#pragma once

#include <radray/render/common.h>

namespace radray::render {

struct RootConstantInfo {
    uint32_t Slot;
    uint32_t Size;
    ShaderStages Stages;
};

struct RootDescriptorInfo {
    uint32_t Slot;
    ResourceType Type;
    ShaderStages Stages;
};

struct DescriptorSetElementInfo {
    uint32_t Slot;
    uint32_t Index;
    uint32_t Count;
    ResourceType Type;
    ShaderStages Stages;
};

class DescriptorSetInfo {
public:
    uint32_t Index;
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

    // virtual uint32_t GetDescriptorSetCount() const noexcept = 0;

    // virtual uint32_t GetConstantBufferSlotCount() const noexcept = 0;

    // virtual uint32_t GetRootConstantCount() const noexcept = 0;

    // virtual radray::vector<DescriptorLayout> GetDescriptorSetLayout(uint32_t set) const noexcept = 0;

    // virtual RootSignatureConstantBufferSlotInfo GetConstantBufferSlotInfo(uint32_t slot) const noexcept = 0;

    // virtual RootSignatureRootConstantSlotInfo GetRootConstantSlotInfo(uint32_t slot) const noexcept = 0;
};

}  // namespace radray::render

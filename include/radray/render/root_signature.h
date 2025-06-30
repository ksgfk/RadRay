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
    vector<DescriptorSetElementInfo> Elements;
};

struct StaticSamplerInfo {
    uint32_t Slot;
    uint32_t Space;
    ShaderStages Stages;
    AddressMode AddressS;
    AddressMode AddressT;
    AddressMode AddressR;
    FilterMode MigFilter;
    FilterMode MagFilter;
    FilterMode MipmapFilter;
    float LodMin;
    float LodMax;
    CompareFunction Compare;
    uint32_t AnisotropyClamp;
    bool HasCompare;
};

struct RootSignatureDescriptor {
    std::span<RootConstantInfo> RootConstants;
    std::span<RootDescriptorInfo> RootDescriptors;
    std::span<DescriptorSetInfo> DescriptorSets;
    std::span<StaticSamplerInfo> StaticSamplers;
};

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::RootSignature; }

    virtual std::span<const RootConstantInfo> GetRootConstants() const noexcept = 0;

    virtual std::span<const RootDescriptorInfo> GetRootDescriptors() const noexcept = 0;

    virtual std::span<const DescriptorSetElementInfo> GetBindDescriptors() const noexcept = 0;
};

}  // namespace radray::render

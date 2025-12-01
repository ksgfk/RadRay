#pragma once

#include <radray/render/common.h>
#include <radray/render/utility.h>
#include <radray/render/dxc.h>

namespace radray::render {

class RootSignatureDescriptorContainer {
public:
    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;
    RootSignatureDescriptorContainer(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer(RootSignatureDescriptorContainer&&) noexcept;
    RootSignatureDescriptorContainer& operator=(const RootSignatureDescriptorContainer&) noexcept;
    RootSignatureDescriptorContainer& operator=(RootSignatureDescriptorContainer&&) noexcept;
    ~RootSignatureDescriptorContainer() noexcept = default;

    const RootSignatureDescriptor& Get() const noexcept;

    friend void swap(RootSignatureDescriptorContainer& lhs, RootSignatureDescriptorContainer& rhs) noexcept;

private:
    void Refresh() noexcept;

    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<RootSignatureDescriptorSet> _descriptorSets;
    RootSignatureDescriptor _desc{};
};

struct HlslRSCombinedBinding {
    std::string_view Name;
    const HlslShaderBufferDesc* CBuffer{nullptr};
    const HlslInputBindDesc* Layout{nullptr};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t Slot{0};
    uint32_t Space{0};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

vector<HlslRSCombinedBinding> MergeHlslShaderBoundResources(std::span<const HlslShaderDesc*> descs) noexcept;

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(std::span<const HlslShaderDesc*> descs) noexcept;

}  // namespace radray::render

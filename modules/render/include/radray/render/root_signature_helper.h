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

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

std::optional<RootSignatureDescriptorContainer> CreateRootSignatureDescriptor(const MergedHlslShaderDesc& desc) noexcept;

}  // namespace radray::render

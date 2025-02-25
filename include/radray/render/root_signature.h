#pragma once

#include <radray/render/common.h>

namespace radray::render {

class RootSignatureConstantBufferSlotInfo {
public:
    radray::string Name;
    size_t Size;
    uint32_t Slot;
};

class RootSignatureRootConstantSlotInfo {
public:
    radray::string Name;
    size_t Size;
    uint32_t Slot;
};

class DescriptorLayout {
public:
    radray::string Name;
    uint32_t Set;
    uint32_t Slot;
    ShaderResourceType Type;
    uint32_t Count;
    size_t CbSize;
};

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;

    virtual uint32_t GetDescriptorSetCount() const noexcept = 0;

    virtual uint32_t GetConstantBufferSlotCount() const noexcept = 0;

    virtual uint32_t GetRootConstantCount() const noexcept = 0;

    virtual radray::vector<DescriptorLayout> GetDescriptorSetLayout(uint32_t set) const noexcept = 0;

    virtual RootSignatureConstantBufferSlotInfo GetConstantBufferSlotInfo(uint32_t slot) const noexcept = 0;

    virtual RootSignatureRootConstantSlotInfo GetRootConstantSlotInfo(uint32_t slot) const noexcept = 0;
};

}  // namespace radray::render

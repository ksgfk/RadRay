#pragma once

#include <radray/render/common.h>
#include <radray/render/descriptor_set.h>

namespace radray::render {

class RootSignatureConstantBufferSlotInfo {
public:
    radray::string Name;
    uint32_t Slot;
};

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;

    virtual uint32_t GetDescriptorSetCount() const noexcept = 0;

    virtual uint32_t GetConstantBufferSlotCount() const noexcept = 0;

    virtual radray::vector<DescriptorLayout> GetDescriptorSetLayout(uint32_t set) const noexcept = 0;

    virtual RootSignatureConstantBufferSlotInfo GetConstantBufferSlotInfo(uint32_t slot) const noexcept = 0;
};

}  // namespace radray::render

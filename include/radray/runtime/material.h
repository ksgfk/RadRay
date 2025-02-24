#pragma once

#include <variant>

#include <radray/types.h>
#include <radray/render/device.h>
#include <radray/render/root_signature.h>
#include <radray/render/pipeline_state.h>
#include <radray/render/resource.h>

namespace radray::runtime {

class Material {
public:
    Material(radray::shared_ptr<render::RootSignature> rootSig) noexcept;

    void SetConstantBufferData(std::string_view name, std::span<const byte> data) noexcept;

    void SetBuffer(std::string_view name, render::BufferView* bv) noexcept;

    void SetTexture(std::string_view name, render::TextureView* tv) noexcept;

private:
    struct DescriptorLayoutIndex {
        size_t Dim1;
        size_t Dim2;
    };
    using Slot = std::variant<render::RootSignatureRootConstantSlotInfo, render::RootSignatureConstantBufferSlotInfo, DescriptorLayoutIndex>;

    radray::shared_ptr<render::RootSignature> _rootSig;
    radray::unordered_map<radray::string, Slot, StringHash, std::equal_to<>> _slots;
    radray::vector<radray::vector<render::DescriptorLayout>> _descLayouts;
};

}  // namespace radray::runtime

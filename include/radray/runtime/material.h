#pragma once

#include <variant>

#include <radray/types.h>
#include <radray/memory.h>
#include <radray/render/device.h>
#include <radray/render/root_signature.h>
#include <radray/render/pipeline_state.h>
#include <radray/render/resource.h>

namespace radray::runtime {

class Material {
public:
    Material(shared_ptr<render::RootSignature> rootSig) noexcept;

    void SetConstantBufferData(std::string_view name, uint32_t index, std::span<const byte> data) noexcept;

    void SetBuffer(std::string_view name, uint32_t index, render::BufferView* bv) noexcept;

    void SetTexture(std::string_view name, uint32_t index, render::TextureView* tv) noexcept;

private:
    struct RootConstSlot {
        render::RootSignatureRootConstantSlotInfo Info;
        size_t CbCacheStart;
    };

    struct CBufferSlot {
        render::RootSignatureConstantBufferSlotInfo Info;
        size_t CbCacheStart;
    };

    struct DescriptorLayoutIndex {
        size_t Dim1;
        size_t Dim2;
        size_t CbCacheStart;
    };

    using Slot = std::variant<RootConstSlot, CBufferSlot, DescriptorLayoutIndex>;

    shared_ptr<render::RootSignature> _rootSig;
    unordered_map<string, Slot, StringHash, std::equal_to<>> _slots;
    vector<vector<render::DescriptorLayout>> _descLayouts;
    Memory _cbCache;
};

}  // namespace radray::runtime

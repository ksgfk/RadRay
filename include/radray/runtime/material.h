#pragma once

#include <variant>

#include <radray/types.h>
#include <radray/memory.h>
#include <radray/render/resource.h>

namespace radray::runtime {

class Material {
public:
    Material(render::RootSignature* rootSig) noexcept;

    void SetConstantBufferData(std::string_view name, uint32_t index, std::span<const byte> data) noexcept;

    void SetResource(std::string_view name, uint32_t index, shared_ptr<render::ResourceView> rv) noexcept;

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
        size_t Index;
        size_t CbCacheStart;
        std::vector<weak_ptr<render::ResourceView>> Views;
    };

    using Slot = std::variant<RootConstSlot, CBufferSlot, DescriptorLayoutIndex>;

    unordered_map<string, Slot, StringHash, std::equal_to<>> _slots;
    vector<render::DescriptorLayout> _descLayouts;
    Memory _cbCache;
};

}  // namespace radray::runtime

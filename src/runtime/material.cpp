#include <radray/runtime/material.h>

namespace radray::runtime {

Material::Material(radray::shared_ptr<render::RootSignature> rootSig) noexcept
    : _rootSig(std::move(rootSig)) {
    for (uint32_t i = 0, all = _rootSig->GetRootConstantCount(); i < all; i++) {
        render::RootSignatureRootConstantSlotInfo slotInfo = _rootSig->GetRootConstantSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, slotInfo);
        if (!isInsert) {
            RADRAY_ERR_LOG("Material exist root constant slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = _rootSig->GetConstantBufferSlotCount(); i < all; i++) {
        render::RootSignatureConstantBufferSlotInfo slotInfo = _rootSig->GetConstantBufferSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, slotInfo);
        if (!isInsert) {
            RADRAY_ERR_LOG("Material exist constant buffer slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = _rootSig->GetDescriptorSetCount(); i < all; i++) {
        radray::vector<render::DescriptorLayout> descLayouts = _rootSig->GetDescriptorSetLayout(i);
        for (size_t j = 0; j < descLayouts.size(); j++) {
            const render::DescriptorLayout& descLayout = descLayouts[j];
            DescriptorLayoutIndex idx{i, j};
            auto [exist, isInsert] = _slots.try_emplace(descLayout.Name, idx);
            if (!isInsert) {
                RADRAY_ERR_LOG("Material exist descriptor slot: {}", exist->first);
            }
        }
        _descLayouts.emplace_back(std::move(descLayouts));
    }
}

void Material::SetConstantBufferData(std::string_view name, std::span<const byte> data) noexcept {
    auto iter = _slots.find(name);
    if (iter == _slots.end()) {
        RADRAY_ERR_LOG("Material not found slot: {}", name);
        return;
    }
}

void Material::SetBuffer(std::string_view name, render::BufferView* bv) noexcept {
}

void Material::SetTexture(std::string_view name, render::TextureView* tv) noexcept {
}

}  // namespace radray::runtime

#include <radray/runtime/material.h>

namespace radray::runtime {

Material::Material(radray::shared_ptr<render::RootSignature> rootSig) noexcept
    : _rootSig(std::move(rootSig)) {
    size_t cbSize = 0;
    for (uint32_t i = 0, all = _rootSig->GetRootConstantCount(); i < all; i++) {
        render::RootSignatureRootConstantSlotInfo slotInfo = _rootSig->GetRootConstantSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, RootConstSlot{slotInfo, 0});
        if (isInsert) {
            auto& rcSlot = std::get<RootConstSlot>(exist->second);
            rcSlot.CbCacheStart = cbSize;
            cbSize += rcSlot.Info.Size;
        } else {
            RADRAY_ERR_LOG("Material exist root constant slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = _rootSig->GetConstantBufferSlotCount(); i < all; i++) {
        render::RootSignatureConstantBufferSlotInfo slotInfo = _rootSig->GetConstantBufferSlotInfo(i);
        auto [exist, isInsert] = _slots.try_emplace(slotInfo.Name, CBufferSlot{slotInfo, 0});
        if (isInsert) {
            auto& cbSlot = std::get<CBufferSlot>(exist->second);
            cbSlot.CbCacheStart = cbSize;
            cbSize += cbSlot.Info.Size;
        } else {
            RADRAY_ERR_LOG("Material exist constant buffer slot: {}", exist->first);
        }
    }
    for (uint32_t i = 0, all = _rootSig->GetDescriptorSetCount(); i < all; i++) {
        radray::vector<render::DescriptorLayout> descLayouts = _rootSig->GetDescriptorSetLayout(i);
        for (size_t j = 0; j < descLayouts.size(); j++) {
            const render::DescriptorLayout& descLayout = descLayouts[j];
            auto [exist, isInsert] = _slots.try_emplace(descLayout.Name, DescriptorLayoutIndex{i, j, 0});
            if (isInsert) {
                auto& descIndex = std::get<DescriptorLayoutIndex>(exist->second);
                if (descLayout.Type == render::ShaderResourceType::CBuffer) {
                    descIndex.CbCacheStart = cbSize;
                    cbSize += descLayout.CbSize * descLayout.Count;
                }
            } else {
                RADRAY_ERR_LOG("Material exist descriptor slot: {}", exist->first);
            }
        }
        _descLayouts.emplace_back(std::move(descLayouts));
    }
    _cbCache.Allocate(cbSize);
}

void Material::SetConstantBufferData(std::string_view name, uint32_t index, std::span<const byte> data) noexcept {
    auto iter = _slots.find(name);
    if (iter == _slots.end()) {
        RADRAY_ERR_LOG("Material not found slot: {}", name);
        return;
    }
    std::visit(
        [this, index, data](auto&& v) noexcept {
            auto copyImpl = [](std::span<const byte> src, std::span<byte> dst, std::string_view tips) noexcept {
                if (dst.size() < src.size()) {
                    RADRAY_ERR_LOG("Material value {} size not match", tips);
                } else {
                    std::copy(src.begin(), src.end(), dst.begin());
                }
            };
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, DescriptorLayoutIndex>) {
                render::DescriptorLayout& layout = _descLayouts[v.Dim1][v.Dim2];
                if (layout.Type == render::ShaderResourceType::CBuffer) {
                    size_t start = v.CbCacheStart + layout.CbSize * index;
                    std::span<byte> dst = _cbCache.GetSpan(start, layout.CbSize);
                    copyImpl(data, dst, layout.Name);
                } else {
                    RADRAY_ERR_LOG("Material value {} is not cbuffer", layout.Name);
                }
            } else {
                std::span<byte> dst = _cbCache.GetSpan(v.CbCacheStart, v.Info.Size);
                copyImpl(data, dst, v.Info.Name);
            }
        },
        iter->second);
}

void Material::SetBuffer(std::string_view name, uint32_t index, render::BufferView* bv) noexcept {
}

void Material::SetTexture(std::string_view name, uint32_t index, render::TextureView* tv) noexcept {
}

}  // namespace radray::runtime

#include <radray/runtime/material_render_proxy.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include <radray/logger.h>
#include <radray/basic_math.h>
#include <radray/runtime/material.h>
#include <radray/runtime/texture_asset.h>

namespace radray {

bool MaterialRenderProxy::Build(render::Device* device, const MaterialInstance& instance) noexcept {
    _constantBuffer.reset();
    _sampler.reset();
    _descriptorSet.reset();

    if (device == nullptr || !instance.IsValid()) {
        return false;
    }
    Material* material = instance.GetMaterial();
    if (material == nullptr || material->GetRootSignature() == nullptr) {
        RADRAY_ERR_LOG("MaterialRenderProxy: material has no root signature");
        return false;
    }
    _setIndex = material->GetMaterialSetIndex();

    // 无 per-material 参数(无 cbuffer 且无贴图)时无需建 descriptor set。
    const MaterialParameterLayout& layout = material->GetParameterLayout();
    std::span<const byte> constData = instance.GetConstantData();
    const bool hasCBuffer = !constData.empty();
    if (!hasCBuffer && layout.GetResourceSlots().empty()) {
        return false;
    }

    // 1) per-material descriptor set。
    auto setOpt = device->CreateDescriptorSet(material->GetRootSignature(), render::DescriptorSetIndex{_setIndex});
    if (!setOpt.HasValue()) {
        RADRAY_ERR_LOG("MaterialRenderProxy: CreateDescriptorSet(set={}) failed", _setIndex);
        return false;
    }
    _descriptorSet = setOpt.Release();

    // 2) 常量缓冲(Upload 堆):按 CBuffer 对齐规整大小,Map 写入打包字节流。
    if (hasCBuffer) {
        const uint32_t align = std::max<uint32_t>(device->GetDetail().CBufferAlignment, 1u);
        const uint64_t bufferSize = radray::Align(static_cast<uint64_t>(constData.size()), static_cast<uint64_t>(align));
        render::BufferDescriptor bufDesc{
            .Size = bufferSize,
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite,
            .Hints = render::ResourceHint::None};
        auto bufOpt = device->CreateBuffer(bufDesc);
        if (!bufOpt.HasValue()) {
            RADRAY_ERR_LOG("MaterialRenderProxy: CreateBuffer(size={}) failed", bufferSize);
            _descriptorSet.reset();
            return false;
        }
        _constantBuffer = bufOpt.Release();

        void* mapped = _constantBuffer->Map(0, bufferSize);
        if (mapped == nullptr) {
            RADRAY_ERR_LOG("MaterialRenderProxy: Map constant buffer failed");
            _constantBuffer.reset();
            _descriptorSet.reset();
            return false;
        }
        std::memcpy(mapped, constData.data(), constData.size());
        _constantBuffer->Unmap(0, bufferSize);

        // 3) 绑常量缓冲到 cbuffer 槽(按反射名)。
        render::BufferBindingDescriptor cbView{};
        cbView.Target = _constantBuffer.get();
        cbView.Range = render::BufferRange{0, bufferSize};
        cbView.Usage = render::BufferViewUsage::CBuffer;
        if (!_descriptorSet->WriteResource(layout.GetConstantBufferName(), cbView)) {
            RADRAY_ERR_LOG("MaterialRenderProxy: WriteResource cbuffer '{}' failed", layout.GetConstantBufferName());
            _constantBuffer.reset();
            _descriptorSet.reset();
            return false;
        }
    }

    // 3) 贴图 + 采样器:遍历参数布局声明的所有资源槽,逐一写入。
    //    D3D12 BindDescriptorSet 要求 set 被【完整写入】(IsFullyWritten),
    //    故每个声明的贴图/采样器槽都必须绑定;任一贴图无法解析则整体失败。
    for (const auto& slot : layout.GetResourceSlots()) {
        if (slot.Kind == MaterialParameterLayout::ResourceKind::Texture) {
            StreamingAssetRef<TextureAsset> texRef = instance.GetTexture(slot.Name).CastTo<TextureAsset>();
            TextureAsset* tex = texRef.Get();
            if (tex == nullptr || !tex->IsValid()) {
                RADRAY_ERR_LOG("MaterialRenderProxy: texture slot '{}' is unbound or not ready", slot.Name);
                _constantBuffer.reset();
                _sampler.reset();
                _descriptorSet.reset();
                return false;
            }
            if (!_descriptorSet->WriteResource(slot.Name, tex->GetSrv())) {
                RADRAY_ERR_LOG("MaterialRenderProxy: WriteResource texture '{}' failed", slot.Name);
                _constantBuffer.reset();
                _sampler.reset();
                _descriptorSet.reset();
                return false;
            }
        } else {  // Sampler
            if (_sampler == nullptr) {
                render::SamplerDescriptor sampDesc{
                    render::AddressMode::Repeat,
                    render::AddressMode::Repeat,
                    render::AddressMode::Repeat,
                    render::FilterMode::Linear,
                    render::FilterMode::Linear,
                    render::FilterMode::Linear,
                    0.0f,
                    std::numeric_limits<float>::max(),
                    std::nullopt,
                    0};
                auto sampOpt = device->CreateSampler(sampDesc);
                if (!sampOpt.HasValue()) {
                    RADRAY_ERR_LOG("MaterialRenderProxy: CreateSampler failed");
                    _constantBuffer.reset();
                    _descriptorSet.reset();
                    return false;
                }
                _sampler = sampOpt.Release();
            }
            if (!_descriptorSet->WriteSampler(slot.Name, _sampler.get())) {
                RADRAY_ERR_LOG("MaterialRenderProxy: WriteSampler '{}' failed", slot.Name);
                _constantBuffer.reset();
                _sampler.reset();
                _descriptorSet.reset();
                return false;
            }
        }
    }

    return true;
}

}  // namespace radray

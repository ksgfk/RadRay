#include <radray/runtime/render/standard_material.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include <radray/logger.h>
#include <radray/basic_math.h>
#include <radray/runtime/shader_variant.h>

namespace radray::srp {

StandardMaterial::StandardMaterial(render::Device* device, Desc desc) noexcept
    : _device(device), _desc(std::move(desc)) {}

StandardMaterial::~StandardMaterial() noexcept = default;

KeywordSet StandardMaterial::MaterialKeywords() const {
    KeywordSet kw;
    if (_desc.Blend == BlendMode::Masked) {
        kw.Add(shader_define::AlphaTest);
    }
    return kw;
}

render::DescriptorSet* StandardMaterial::GetDescriptorSet(render::RootSignature* rootSig) const {
    if (rootSig == nullptr || _device == nullptr) {
        return nullptr;
    }
    // 命中缓存(含"无 space1"的合法空结果)。
    for (CacheEntry& entry : _cache) {
        if (entry.RootSig == rootSig && entry.Built) {
            return entry.Set.get();
        }
    }
    return Build(rootSig);
}

render::DescriptorSet* StandardMaterial::Build(render::RootSignature* rootSig) const {
    // 判定本 rootSig(变体)里有哪些 space1 槽。depth-only / shadow 变体通常一个都没有。
    const bool hasCBuffer = !_desc.CBufferData.empty() &&
                            rootSig->FindParameterId(_desc.CBufferName).has_value();
    bool hasAnyTexture = false;
    for (const auto& [name, view] : _desc.Textures) {
        if (rootSig->FindParameterId(name).has_value()) {
            hasAnyTexture = true;
            break;
        }
    }
    const bool hasSampler = !_desc.SamplerName.empty() &&
                            rootSig->FindParameterId(_desc.SamplerName).has_value();

    if (!hasCBuffer && !hasAnyTexture && !hasSampler) {
        // 本变体无 per-material 绑定 —— 合法空结果,缓存避免反复探测。
        CacheEntry empty{};
        empty.RootSig = rootSig;
        empty.Built = true;
        _cache.push_back(std::move(empty));
        return nullptr;
    }

    auto setOpt = _device->CreateDescriptorSet(rootSig, _desc.SetIndex);
    if (!setOpt.HasValue()) {
        RADRAY_ERR_LOG("StandardMaterial: CreateDescriptorSet(set={}) failed", _desc.SetIndex.Value);
        return nullptr;  // 不缓存,下帧重试
    }
    unique_ptr<render::DescriptorSet> set = setOpt.Release();

    unique_ptr<render::Buffer> cbuffer;
    if (hasCBuffer) {
        const uint32_t align = std::max<uint32_t>(_device->GetDetail().CBufferAlignment, 1u);
        const uint64_t bufferSize = radray::Align(static_cast<uint64_t>(_desc.CBufferData.size()),
                                                  static_cast<uint64_t>(align));
        render::BufferDescriptor bufDesc{
            .Size = bufferSize,
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite,
            .Hints = render::ResourceHint::None};
        auto bufOpt = _device->CreateBuffer(bufDesc);
        if (!bufOpt.HasValue()) {
            RADRAY_ERR_LOG("StandardMaterial: CreateBuffer(size={}) failed", bufferSize);
            return nullptr;
        }
        cbuffer = bufOpt.Release();
        void* mapped = cbuffer->Map(0, bufferSize);
        if (mapped == nullptr) {
            RADRAY_ERR_LOG("StandardMaterial: Map cbuffer failed");
            return nullptr;
        }
        std::memcpy(mapped, _desc.CBufferData.data(), _desc.CBufferData.size());
        cbuffer->Unmap(0, bufferSize);

        render::BufferBindingDescriptor cbView{};
        cbView.Target = cbuffer.get();
        cbView.Range = render::BufferRange{0, bufferSize};
        cbView.Usage = render::BufferViewUsage::CBuffer;
        if (!set->WriteResource(std::string_view{_desc.CBufferName}, cbView)) {
            RADRAY_ERR_LOG("StandardMaterial: WriteResource cbuffer '{}' failed", _desc.CBufferName);
            return nullptr;
        }
    }

    // 贴图:D3D12 要求 set 被完整写入,故 rootSig 声明的每个贴图槽都必须能解析到非空 SRV。
    for (const auto& [name, view] : _desc.Textures) {
        if (!rootSig->FindParameterId(name).has_value()) {
            continue;  // 本变体没这个槽
        }
        if (view == nullptr) {
            RADRAY_ERR_LOG("StandardMaterial: texture slot '{}' not ready", name);
            return nullptr;  // 资产未就绪,下帧重试
        }
        if (!set->WriteResource(std::string_view{name}, view)) {
            RADRAY_ERR_LOG("StandardMaterial: WriteResource texture '{}' failed", name);
            return nullptr;
        }
    }

    if (hasSampler) {
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
            auto sampOpt = _device->CreateSampler(sampDesc);
            if (!sampOpt.HasValue()) {
                RADRAY_ERR_LOG("StandardMaterial: CreateSampler failed");
                return nullptr;
            }
            _sampler = sampOpt.Release();
        }
        if (!set->WriteSampler(std::string_view{_desc.SamplerName}, _sampler.get())) {
            RADRAY_ERR_LOG("StandardMaterial: WriteSampler '{}' failed", _desc.SamplerName);
            return nullptr;
        }
    }

    CacheEntry entry{};
    entry.RootSig = rootSig;
    entry.Set = std::move(set);
    entry.CBuffer = std::move(cbuffer);
    entry.Built = true;
    render::DescriptorSet* result = entry.Set.get();
    _cache.push_back(std::move(entry));
    return result;
}

}  // namespace radray::srp

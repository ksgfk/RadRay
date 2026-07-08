#include <radray/runtime/render_framework/sampler_cache.h>

#include <radray/logger.h>

namespace radray {

SamplerKey BuildSamplerKey(const render::SamplerDescriptor& desc) noexcept {
    SamplerKey key{};  // 清零, 保证 padding 恒为 0 (PodHasher/PodEqual 要求)
    key.AddressS = static_cast<int32_t>(desc.AddressS);
    key.AddressT = static_cast<int32_t>(desc.AddressT);
    key.AddressR = static_cast<int32_t>(desc.AddressR);
    key.MinFilter = static_cast<int32_t>(desc.MinFilter);
    key.MagFilter = static_cast<int32_t>(desc.MagFilter);
    key.MipmapFilter = static_cast<int32_t>(desc.MipmapFilter);
    key.LodMin = desc.LodMin;
    key.LodMax = desc.LodMax;
    key.HasCompare = desc.Compare.has_value() ? 1u : 0u;
    key.Compare = desc.Compare.has_value() ? static_cast<int32_t>(*desc.Compare) : 0;
    key.AnisotropyClamp = desc.AnisotropyClamp;
    return key;
}

SamplerCache::SamplerCache(render::Device* device) noexcept
    : _device(device) {}

Nullable<render::Sampler*> SamplerCache::GetOrCreate(const render::SamplerDescriptor& desc) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }
    const SamplerKey key = BuildSamplerKey(desc);
    if (auto it = _cache.find(key); it != _cache.end()) {
        return it->second.get();
    }
    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        RADRAY_ERR_LOG("SamplerCache::GetOrCreate: failed to create sampler");
        return nullptr;
    }
    render::Sampler* raw = samplerOpt.Get();
    _cache.emplace(key, samplerOpt.Release());
    return raw;
}

}  // namespace radray

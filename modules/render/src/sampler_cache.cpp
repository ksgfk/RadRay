#include <radray/render/sampler_cache.h>

#include <radray/hash.h>

std::size_t std::hash<radray::render::SamplerDescriptor>::operator()(
    const radray::render::SamplerDescriptor& desc) const noexcept {
    radray::HashCode hash;
    hash.Add(static_cast<radray::int32_t>(desc.AddressS));
    hash.Add(static_cast<radray::int32_t>(desc.AddressT));
    hash.Add(static_cast<radray::int32_t>(desc.AddressR));
    hash.Add(static_cast<radray::int32_t>(desc.MinFilter));
    hash.Add(static_cast<radray::int32_t>(desc.MagFilter));
    hash.Add(static_cast<radray::int32_t>(desc.MipmapFilter));
    hash.Add(desc.LodMin == 0 ? 0.0f : desc.LodMin);
    hash.Add(desc.LodMax == 0 ? 0.0f : desc.LodMax);
    hash.Add(desc.Compare.has_value());
    hash.Add(desc.Compare.has_value() ? static_cast<radray::int32_t>(*desc.Compare) : 0);
    hash.Add(desc.AnisotropyClamp);
    hash.Add(desc.MipLodBias == 0 ? 0.0f : desc.MipLodBias);
    hash.Add(static_cast<uint32_t>(desc.BorderColor));
    hash.Add(static_cast<uint32_t>(desc.Reduction));
    hash.Add(desc.UnnormalizedCoordinates);
    return hash.ToHashCode();
}

namespace radray::render {

SamplerCache::SamplerCache(Device* device) noexcept
    : _device(device) {}

SamplerCache::~SamplerCache() noexcept {
    Clear();
}

Nullable<Sampler*> SamplerCache::GetOrCreate(
    const SamplerDescriptor& desc) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }
    if (const auto it = _cache.find(desc); it != _cache.end()) {
        return it->second.get();
    }
    auto sampler = _device->CreateSampler(desc);
    if (!sampler.HasValue()) {
        return nullptr;
    }
    Sampler* result = sampler.Get();
    _cache.emplace(desc, sampler.Release());
    return result;
}

void SamplerCache::Clear() noexcept {
    for (auto& [desc, sampler] : _cache) {
        (void)desc;
        if (sampler != nullptr) {
            sampler->Destroy();
        }
    }
    _cache.clear();
    _device = nullptr;
}

}  // namespace radray::render

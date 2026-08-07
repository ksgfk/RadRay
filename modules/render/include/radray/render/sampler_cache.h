#pragma once

#include <functional>

#include <radray/render/rhi.h>

namespace std {

template <>
struct hash<radray::render::SamplerDescriptor> {
    size_t operator()(const radray::render::SamplerDescriptor& desc) const noexcept;
};

}  // namespace std

namespace radray::render {

class SamplerCache final {
public:
    explicit SamplerCache(Device* device) noexcept;

    ~SamplerCache() noexcept;

    SamplerCache(const SamplerCache&) = delete;
    SamplerCache(SamplerCache&&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    SamplerCache& operator=(SamplerCache&&) = delete;

    Nullable<Sampler*> GetOrCreate(const SamplerDescriptor& desc) noexcept;

    void Clear() noexcept;

private:
    Device* _device;
    unordered_map<SamplerDescriptor, unique_ptr<Sampler>> _cache;
};

}  // namespace radray::render

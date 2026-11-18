#pragma once

#include <cstddef>
#include <functional>

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>

namespace std {

template <>
struct hash<radray::gpu::SamplerDescriptor> {
    size_t operator()(const radray::gpu::SamplerDescriptor& desc) const noexcept;
};

}  // namespace std

namespace radray::gpu {

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

}  // namespace radray::gpu

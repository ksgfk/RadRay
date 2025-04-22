#pragma once

#include <radray/render/common.h>

namespace radray::render {

struct SamplerDescriptor {
    AddressMode AddressS;
    AddressMode AddressT;
    AddressMode AddressR;
    FilterMode MigFilter;
    FilterMode MagFilter;
    FilterMode MipmapFilter;
    float LodMin;
    float LodMax;
    CompareFunction Compare;
    uint32_t AnisotropyClamp;
    bool HasCompare;
};

bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;
bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept;

class Sampler : public RenderBase {
public:
    ~Sampler() noexcept override = default;

    virtual SamplerDescriptor GetDesc() const noexcept = 0;
};

}  // namespace radray::render

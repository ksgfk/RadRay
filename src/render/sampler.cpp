#include <radray/render/sampler.h>

namespace radray::render {

bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept {
    return lhs.AddressS == rhs.AddressS &&
           lhs.AddressT == rhs.AddressT &&
           lhs.AddressR == rhs.AddressR &&
           lhs.MigFilter == rhs.MigFilter &&
           lhs.MagFilter == rhs.MagFilter &&
           lhs.MipmapFilter == rhs.MipmapFilter &&
           lhs.LodMin == rhs.LodMin &&
           lhs.LodMax == rhs.LodMax &&
           lhs.Compare == rhs.Compare &&
           lhs.AnisotropyClamp == rhs.AnisotropyClamp &&
           lhs.HasCompare == rhs.HasCompare;
}

bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace radray::render

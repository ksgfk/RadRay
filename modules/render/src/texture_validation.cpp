#include <radray/render/rhi.h>

#include <algorithm>
#include <bit>

namespace radray::render {

bool IsValidTextureSupportQuery(const TextureSupportQuery& query) noexcept {
    if (!EnumContains(query.Dimension) || query.Dimension == TextureDimension::UNKNOWN ||
        !EnumContains(query.Format) || query.Format == TextureFormat::UNKNOWN ||
        !query.Usage || (query.Usage.value() & ~uint32_t{127}) != 0) {
        return false;
    }
    const bool depthUsage = bool(query.Usage & (TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite));
    const bool depthFormat = IsDepthStencilFormat(query.Format);
    if (depthUsage && !depthFormat) return false;
    if (depthFormat && (query.Usage & (TextureUse::RenderTarget | TextureUse::UnorderedAccess))) return false;
    if (depthUsage && query.Dimension == TextureDimension::Dim3D) return false;
    return true;
}

TextureDescriptorValidationResult ValidateTextureDescriptor(
    const TextureDescriptor& desc, const RenderDeviceCapabilities& capabilities, const TextureSupport& support) {
    if (!IsValidTextureSupportQuery({desc.Dim, desc.Format, desc.Usage})) {
        return {false, "Dimension, Format or Usage is invalid or incompatible"};
    }
    if (desc.Memory != MemoryType::Device || (desc.Hints & ~ResourceHints{ResourceHint::Dedicated})) {
        return {false, "Texture Memory must be Device; only the Dedicated allocation hint is supported"};
    }
    if (desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0 || desc.MipLevels == 0) {
        return {false, "Width, Height, DepthOrArraySize and MipLevels must be nonzero"};
    }
    if (!EnumContains(static_cast<SampleCount>(desc.SampleCount))) {
        return {false, "SampleCount must be 1, 2, 4, 8 or 16"};
    }
    const bool is1D = desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim1DArray;
    const bool is3D = desc.Dim == TextureDimension::Dim3D;
    const bool isCube = desc.Dim == TextureDimension::Cube || desc.Dim == TextureDimension::CubeArray;
    if ((is1D && desc.Height != 1) ||
        ((desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D) && desc.DepthOrArraySize != 1) ||
        (isCube && (desc.Width != desc.Height || desc.DepthOrArraySize % 6 != 0)) ||
        (desc.Dim == TextureDimension::Cube && desc.DepthOrArraySize != 6)) {
        return {false, "Extent or array layer count is incompatible with Dimension"};
    }
    if (desc.SampleCount > 1 &&
        (desc.MipLevels != 1 || (desc.Dim != TextureDimension::Dim2D && desc.Dim != TextureDimension::Dim2DArray) ||
         desc.Usage.HasFlag(TextureUse::UnorderedAccess))) {
        return {false, "MSAA requires 2D/2DArray, MipLevels=1 and no UnorderedAccess usage"};
    }
    const auto& limits = capabilities.Limits;
    const uint32_t dimensionLimit = is1D ? limits.MaxTexture1DDimension : is3D ? limits.MaxTexture3DDimension
                                                                               : limits.MaxTexture2DDimension;
    if (desc.Width > dimensionLimit || desc.Height > dimensionLimit ||
        desc.DepthOrArraySize > (is3D ? limits.MaxTexture3DDimension : limits.MaxTextureArrayLayers)) {
        return {false, "Extent or array layer count exceeds DeviceLimits"};
    }
    const uint32_t maxDimension = std::max({desc.Width, desc.Height, is3D ? desc.DepthOrArraySize : 1u});
    if (desc.MipLevels > static_cast<uint32_t>(std::bit_width(maxDimension))) {
        return {false, "MipLevels exceeds the complete mip chain for this extent"};
    }
    if (!support.Supported || !support.SampleCounts.HasFlag(static_cast<SampleCount>(desc.SampleCount))) {
        return {false, "Dimension/Format/Usage/SampleCount is not supported by the device"};
    }
    if (desc.Width > support.MaxWidth || desc.Height > support.MaxHeight ||
        (is3D ? desc.DepthOrArraySize > support.MaxDepth : desc.DepthOrArraySize > support.MaxArrayLayers) ||
        desc.MipLevels > support.MaxMipLevels) {
        return {false, "Extent, layers or mip count exceeds the format/usage support limits"};
    }
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
        uint64_t mipBytes = GetTextureFormatBytesPerPixel(desc.Format);
        const uint32_t factors[]{std::max(1u, desc.Width >> mip), std::max(1u, desc.Height >> mip),
                                 is3D ? std::max(1u, desc.DepthOrArraySize >> mip) : desc.DepthOrArraySize, desc.SampleCount};
        for (const auto factor : factors) {
            if (mipBytes > std::numeric_limits<uint64_t>::max() / factor) return {false, "Texture size overflows uint64_t"};
            mipBytes *= factor;
        }
        if (mipBytes > support.MaxResourceSize || bytes > support.MaxResourceSize - mipBytes) {
            return {false, "Texture size exceeds format/usage MaxResourceSize"};
        }
        bytes += mipBytes;
    }
    return {true, {}};
}

TextureDescriptorValidationResult ValidateTextureDescriptor(const TextureDescriptor& desc, const Device& device) {
    const TextureSupportQuery query{desc.Dim, desc.Format, desc.Usage};
    return ValidateTextureDescriptor(desc, device.GetCapabilities(), device.QueryTextureSupport(query));
}

std::optional<SubresourceRange> NormalizeSubresourceRange(const TextureDescriptor& desc, SubresourceRange range) noexcept {
    const uint32_t layers = desc.Dim == TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
    if (range.BaseArrayLayer >= layers || range.BaseMipLevel >= desc.MipLevels) return std::nullopt;
    if (range.ArrayLayerCount == SubresourceRange::All) range.ArrayLayerCount = layers - range.BaseArrayLayer;
    if (range.MipLevelCount == SubresourceRange::All) range.MipLevelCount = desc.MipLevels - range.BaseMipLevel;
    if (range.ArrayLayerCount == 0 || range.ArrayLayerCount > layers - range.BaseArrayLayer ||
        range.MipLevelCount == 0 || range.MipLevelCount > desc.MipLevels - range.BaseMipLevel) return std::nullopt;
    return range;
}

std::string_view format_as(SampleCount value) noexcept {
    return EnumNameOr(value);
}

}  // namespace radray::render

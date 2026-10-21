#pragma once

#include <radray/render/rhi.h>

namespace radray::render::detail {

inline vector<std::pair<TextureSupportQuery, TextureSupport>> CacheCommonTextureSupport(const Device& device) {
    vector<std::pair<TextureSupportQuery, TextureSupport>> result;
    const TextureUses usages[]{TextureUse::Resource, TextureUse::Resource | TextureUse::CopyDestination,
                               TextureUse::RenderTarget | TextureUse::CopySource,
                               TextureUse::Resource | TextureUse::RenderTarget,
                               TextureUse::Resource | TextureUse::UnorderedAccess,
                               TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite};
    for (int32_t format = 1; format <= static_cast<int32_t>(TextureFormat::D32_FLOAT_S8_UINT); ++format) {
        for (const auto usage : usages) {
            TextureSupportQuery query{TextureDimension::Dim2D, static_cast<TextureFormat>(format), usage};
            result.emplace_back(query, device.QueryTextureSupport(query));
        }
    }
    return result;
}

}  // namespace radray::render::detail

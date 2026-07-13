#pragma once

#include <array>

#include <radray/runtime/material_property.h>
#include <radray/runtime/texture_asset.h>
#include <radray/types.h>

namespace radray {

class AssetManager;
class FrameUploadScheduler;

enum class ShaderDefaultResourceStatus {
    Ready,
    Pending,
    Invalid,
};

struct ShaderDefaultTextureResult {
    ShaderDefaultResourceStatus Status{ShaderDefaultResourceStatus::Invalid};
    Nullable<render::TextureView*> View{nullptr};
};

class ShaderDefaultResourceLibrary {
public:
    ShaderDefaultResourceLibrary() noexcept = default;
    ShaderDefaultResourceLibrary(const ShaderDefaultResourceLibrary&) = delete;
    ShaderDefaultResourceLibrary& operator=(const ShaderDefaultResourceLibrary&) = delete;

    bool Initialize(AssetManager& assets, FrameUploadScheduler& uploads);
    ShaderDefaultTextureResult ResolveTexture(ShaderDefaultTexture semantic) const noexcept;

private:
    static constexpr size_t kTextureCount = 5;
    static size_t TextureIndex(ShaderDefaultTexture semantic) noexcept;

    std::array<StreamingAssetRef<TextureAsset>, kTextureCount> _textures{};
};

}  // namespace radray

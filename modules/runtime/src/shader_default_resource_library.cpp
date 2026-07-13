#include <radray/runtime/shader_default_resource_library.h>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/image_asset.h>

namespace radray {

size_t ShaderDefaultResourceLibrary::TextureIndex(ShaderDefaultTexture semantic) noexcept {
    return static_cast<size_t>(semantic);
}

bool ShaderDefaultResourceLibrary::Initialize(
    AssetManager& assets,
    FrameUploadScheduler& uploads) {
    const auto load = [&](ShaderDefaultTexture semantic,
                          std::string_view name,
                          uint8_t r,
                          uint8_t g,
                          uint8_t b,
                          bool srgb) {
        _textures[TextureIndex(semantic)] = LoadTextureAssetFromImage(
            assets,
            uploads,
            Guid::NewGuid(),
            string{name},
            MakeSolidImage(r, g, b, 255),
            TextureAssetLoadOptions{.Srgb = srgb});
        return _textures[TextureIndex(semantic)].IsValid();
    };

    bool valid = true;
    valid = load(ShaderDefaultTexture::WhiteLinear, "shader_default_white_linear", 255, 255, 255, false) && valid;
    valid = load(ShaderDefaultTexture::WhiteSrgb, "shader_default_white_srgb", 255, 255, 255, true) && valid;
    valid = load(ShaderDefaultTexture::BlackLinear, "shader_default_black_linear", 0, 0, 0, false) && valid;
    valid = load(ShaderDefaultTexture::BlackSrgb, "shader_default_black_srgb", 0, 0, 0, true) && valid;
    valid = load(ShaderDefaultTexture::FlatNormal, "shader_default_flat_normal", 128, 128, 255, false) && valid;
    return valid;
}

ShaderDefaultTextureResult ShaderDefaultResourceLibrary::ResolveTexture(
    ShaderDefaultTexture semantic) const noexcept {
    const StreamingAssetRef<TextureAsset>& ref = _textures[TextureIndex(semantic)];
    TextureAsset* texture = ref.Get();
    if (texture != nullptr && texture->GetSrv() != nullptr) {
        return ShaderDefaultTextureResult{
            .Status = ShaderDefaultResourceStatus::Ready,
            .View = texture->GetSrv()};
    }
    if (ref.IsValid() && !ref.IsCompleted()) {
        return ShaderDefaultTextureResult{.Status = ShaderDefaultResourceStatus::Pending};
    }
    return ShaderDefaultTextureResult{.Status = ShaderDefaultResourceStatus::Invalid};
}

}  // namespace radray

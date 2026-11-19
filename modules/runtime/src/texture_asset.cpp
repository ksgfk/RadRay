#include <radray/runtime/texture_asset.h>

#include <utility>

namespace radray {

std::optional<TextureAsset> TextureAsset::Create(IntrusivePtr<GpuTexture> texture) {
    if (!texture || !texture->GetDefaultView()) return std::nullopt;
    return TextureAsset(std::move(texture));
}

TextureAsset::TextureAsset(IntrusivePtr<GpuTexture> texture) noexcept : _texture(std::move(texture)) {}

AssetReloadDecision TextureAsset::ClassifyReload(const TextureAsset& candidate) const {
    if (_texture == candidate._texture) return {};
    return {AssetReloadKind::Breaking, "texture object changed"};
}

const IntrusivePtr<GpuTexture>& TextureAsset::GetTexture() const noexcept { return _texture; }
gpu::TextureView* TextureAsset::GetDefaultView() const noexcept { return _texture->GetDefaultView().Get(); }

}  // namespace radray

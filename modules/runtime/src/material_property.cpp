#include <radray/runtime/material_property.h>

#include <cstring>

namespace radray {

ShaderPropertyKind GetMaterialPropertyKind(const MaterialPropertyValue& value) noexcept {
    return std::visit(
        [](const auto& v) noexcept {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                return ShaderPropertyKind::Float;
            } else if constexpr (std::is_same_v<T, Eigen::Vector4f>) {
                return ShaderPropertyKind::Vector;
            } else if constexpr (std::is_same_v<T, vector<byte>>) {
                return ShaderPropertyKind::Bytes;
            } else if constexpr (
                std::is_same_v<T, StreamingAssetRef<TextureAsset>> ||
                std::is_same_v<T, TextureSubViewRef> ||
                std::is_same_v<T, ShaderDefaultTexture>) {
                return ShaderPropertyKind::Texture;
            } else {
                static_assert(std::is_same_v<T, render::SamplerDescriptor>);
                return ShaderPropertyKind::Sampler;
            }
        },
        value);
}

namespace {

bool TextureRefsEqual(
    const StreamingAssetRef<TextureAsset>& lhs,
    const StreamingAssetRef<TextureAsset>& rhs) noexcept {
    return lhs.GetAssetId() == rhs.GetAssetId() && lhs.GetHandle() == rhs.GetHandle();
}

bool SubViewsEqual(const TextureSubViewDesc& lhs, const TextureSubViewDesc& rhs) noexcept {
    const TextureViewKey lhsKey = BuildTextureViewKey(lhs);
    const TextureViewKey rhsKey = BuildTextureViewKey(rhs);
    return std::memcmp(&lhsKey, &rhsKey, sizeof(TextureViewKey)) == 0;
}

}  // namespace

bool MaterialPropertyValuesEqual(
    const MaterialPropertyValue& lhs,
    const MaterialPropertyValue& rhs) noexcept {
    if (lhs.index() != rhs.index()) {
        return false;
    }
    return std::visit(
        [](const auto& a, const auto& b) noexcept {
            using A = std::decay_t<decltype(a)>;
            using B = std::decay_t<decltype(b)>;
            if constexpr (!std::is_same_v<A, B>) {
                return false;
            } else if constexpr (std::is_same_v<A, Eigen::Vector4f>) {
                return std::memcmp(a.data(), b.data(), sizeof(float) * 4) == 0;
            } else if constexpr (std::is_same_v<A, StreamingAssetRef<TextureAsset>>) {
                return TextureRefsEqual(a, b);
            } else if constexpr (std::is_same_v<A, TextureSubViewRef>) {
                return TextureRefsEqual(a.Texture, b.Texture) && SubViewsEqual(a.SubView, b.SubView);
            } else {
                return a == b;
            }
        },
        lhs,
        rhs);
}

}  // namespace radray

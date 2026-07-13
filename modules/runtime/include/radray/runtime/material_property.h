#pragma once

#include <optional>
#include <variant>

#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/texture_asset.h>
#include <radray/types.h>

namespace radray {

enum class ShaderDefaultTexture {
    WhiteLinear,
    WhiteSrgb,
    BlackLinear,
    BlackSrgb,
    FlatNormal,
};

struct TextureSubViewRef {
    StreamingAssetRef<TextureAsset> Texture{};
    TextureSubViewDesc SubView{};
};

using MaterialPropertyValue = std::variant<
    float,
    Eigen::Vector4f,
    vector<byte>,
    StreamingAssetRef<TextureAsset>,
    TextureSubViewRef,
    ShaderDefaultTexture,
    render::SamplerDescriptor>;

enum class ShaderPropertyKind {
    Float,
    Vector,
    Bytes,
    Texture,
    Sampler,
};

ShaderPropertyKind GetMaterialPropertyKind(const MaterialPropertyValue& value) noexcept;
bool MaterialPropertyValuesEqual(
    const MaterialPropertyValue& lhs,
    const MaterialPropertyValue& rhs) noexcept;

}  // namespace radray

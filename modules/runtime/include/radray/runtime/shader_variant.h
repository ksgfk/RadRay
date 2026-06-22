#pragma once

#include <span>
#include <string_view>

#include <radray/types.h>

namespace radray {

namespace shader_define {

inline constexpr std::string_view AlphaTest = "ALPHA_TEST";
inline constexpr std::string_view ShadowCaster = "SHADOW_CASTER";

}  // namespace shader_define

struct ShaderDefine {
    string Name{};
    string Value{};

    friend bool operator==(const ShaderDefine&, const ShaderDefine&) noexcept = default;
};

/// per-draw 的像素着色器维度。把原先两个独立布尔尾参
/// (needPixelShader / alphaClipOnlyPixelShader)收敛成一个类型化枚举:
/// 原本的 (need=false, alphaClipOnly=true) 是无意义组合,类型化后从根上消除;
/// PSO / shader 缓存 key 也因此自解释。对应计划独立项 D 的「变体维度类型化」。
enum class PixelShaderMode : uint8_t {
    /// 不绑定像素着色器:depth-only 不透明(Pre-Z / 不透明阴影)。
    None,
    /// 仅为 alpha-clip 绑定像素着色器:depth-only 但材质带 alpha test(masked 阴影 / masked Pre-Z)。
    AlphaClipOnly,
    /// 完整颜色像素着色器:base / transparent pass。
    FullColor,
};

/// 由 pass 的「是否写颜色」与材质「是否 masked」推导像素着色器维度。
/// 等价于原 scene_renderer 里的两行布尔推导(need = writeColor||masked;
/// alphaClipOnly = !writeColor && masked),集中到此处避免散落。
constexpr PixelShaderMode ResolvePixelShaderMode(bool writeColor, bool materialMasked) noexcept {
    if (writeColor) {
        return PixelShaderMode::FullColor;
    }
    return materialMasked ? PixelShaderMode::AlphaClipOnly : PixelShaderMode::None;
}

/// 是否需要编译 / 绑定像素着色器(原 needPixelShader)。
constexpr bool NeedsPixelShader(PixelShaderMode mode) noexcept {
    return mode != PixelShaderMode::None;
}

/// 是否只用 alpha-clip(depth)像素着色器入口(原 alphaClipOnlyPixelShader)。
constexpr bool IsAlphaClipOnly(PixelShaderMode mode) noexcept {
    return mode == PixelShaderMode::AlphaClipOnly;
}

class ShaderVariantKey {
public:
    ShaderVariantKey() noexcept = default;
    explicit ShaderVariantKey(std::span<const ShaderDefine> defines);

    void Add(std::string_view name, std::string_view value = {});
    void Merge(const ShaderVariantKey& other);

    std::span<const ShaderDefine> Defines() const noexcept { return _defines; }
    void AppendSignature(string& out) const;

    friend bool operator==(const ShaderVariantKey&, const ShaderVariantKey&) noexcept = default;

private:
    void Normalize();

    vector<ShaderDefine> _defines;
};

struct ShaderVariantKeyHash {
    size_t operator()(const ShaderVariantKey& key) const noexcept;
};

}  // namespace radray

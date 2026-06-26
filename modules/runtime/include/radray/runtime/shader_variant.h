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

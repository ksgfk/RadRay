#include <radray/runtime/shader_variant.h>

#include <algorithm>

#include <radray/utility.h>

namespace radray {

ShaderVariantKey::ShaderVariantKey(std::span<const ShaderDefine> defines)
    : _defines(defines.begin(), defines.end()) {
    Normalize();
}

void ShaderVariantKey::Add(std::string_view name, std::string_view value) {
    if (name.empty()) {
        return;
    }
    _defines.emplace_back(ShaderDefine{string{name}, string{value}});
    Normalize();
}

void ShaderVariantKey::Merge(const ShaderVariantKey& other) {
    _defines.insert(_defines.end(), other._defines.begin(), other._defines.end());
    Normalize();
}

void ShaderVariantKey::AppendSignature(string& out) const {
    for (const ShaderDefine& define : _defines) {
        out += "|D:";
        out += define.Name;
        if (!define.Value.empty()) {
            out += "=";
            out += define.Value;
        }
    }
}

void ShaderVariantKey::Normalize() {
    std::erase_if(_defines, [](const ShaderDefine& define) noexcept {
        return define.Name.empty();
    });
    std::sort(
        _defines.begin(),
        _defines.end(),
        [](const ShaderDefine& lhs, const ShaderDefine& rhs) noexcept {
            return lhs.Name < rhs.Name;
        });
    auto out = _defines.begin();
    for (auto it = _defines.begin(); it != _defines.end(); ++it) {
        if (out != _defines.begin() && std::prev(out)->Name == it->Name) {
            if (std::prev(out)->Value.empty()) {
                std::prev(out)->Value = it->Value;
            }
            continue;
        }
        if (out != it) {
            *out = std::move(*it);
        }
        ++out;
    }
    _defines.erase(out, _defines.end());
}

size_t ShaderVariantKeyHash::operator()(const ShaderVariantKey& key) const noexcept {
    size_t seed = 0;
    std::hash<std::string_view> hashString;
    for (const ShaderDefine& define : key.Defines()) {
        HashCombine(seed, hashString(std::string_view{define.Name}));
        HashCombine(seed, hashString(std::string_view{define.Value}));
    }
    return seed;
}

}  // namespace radray

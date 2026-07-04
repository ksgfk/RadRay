#include <radray/runtime/material_asset.h>

#include <utility>

#include <radray/logger.h>

namespace radray {

MaterialAsset::MaterialAsset(ShaderAsset* shader) noexcept
    : _shader(shader) {}

MaterialAsset::~MaterialAsset() noexcept = default;

void MaterialAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    // MaterialAsset 不拥有 GPU 资源 (texture/sampler 为非拥有指针,
    // 变体/参数表由各自缓存持有)。仅清空 CPU 状态。
    _properties.clear();
    _enabledKeywords.clear();
    _keywordIndex.clear();
    _shader = nullptr;
}

AssetTypeId MaterialAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<MaterialAsset>;
}

void MaterialAsset::SetFloat(std::string_view name, float value) noexcept {
    _properties[string{name}] = value;
}

void MaterialAsset::SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept {
    _properties[string{name}] = value;
}

void MaterialAsset::SetTexture(std::string_view name, render::TextureView* view) noexcept {
    _properties[string{name}] = view;
}

void MaterialAsset::SetSampler(std::string_view name, render::Sampler* sampler) noexcept {
    _properties[string{name}] = sampler;
}

std::optional<MaterialPropertyValue> MaterialAsset::GetProperty(std::string_view name) const noexcept {
    auto it = _properties.find(string{name});
    if (it == _properties.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<float> MaterialAsset::GetFloat(std::string_view name) const noexcept {
    auto it = _properties.find(string{name});
    if (it == _properties.end()) {
        return std::nullopt;
    }
    if (const float* v = std::get_if<float>(&it->second)) {
        return *v;
    }
    return std::nullopt;
}

std::optional<Eigen::Vector4f> MaterialAsset::GetVector(std::string_view name) const noexcept {
    auto it = _properties.find(string{name});
    if (it == _properties.end()) {
        return std::nullopt;
    }
    if (const Eigen::Vector4f* v = std::get_if<Eigen::Vector4f>(&it->second)) {
        return *v;
    }
    return std::nullopt;
}

void MaterialAsset::EnableKeyword(std::string_view name) noexcept {
    if (_keywordIndex.find(string{name}) != _keywordIndex.end()) {
        return;  // 已启用
    }
    const uint32_t idx = static_cast<uint32_t>(_enabledKeywords.size());
    _enabledKeywords.emplace_back(name);
    _keywordIndex.emplace(string{name}, idx);
}

void MaterialAsset::DisableKeyword(std::string_view name) noexcept {
    auto it = _keywordIndex.find(string{name});
    if (it == _keywordIndex.end()) {
        return;
    }
    const uint32_t removed = it->second;
    _enabledKeywords.erase(_enabledKeywords.begin() + removed);
    _keywordIndex.erase(it);
    // 修正被删元素之后所有索引。
    for (auto& kv : _keywordIndex) {
        if (kv.second > removed) {
            --kv.second;
        }
    }
}

bool MaterialAsset::IsKeywordEnabled(std::string_view name) const noexcept {
    return _keywordIndex.find(string{name}) != _keywordIndex.end();
}

Nullable<const render::CompiledShaderVariant*> MaterialAsset::ResolveVariant(
    render::ShaderVariantCache& cache,
    uint32_t passIndex,
    render::HlslShaderModel sm) noexcept {
    if (_shader == nullptr) {
        RADRAY_ERR_LOG("MaterialAsset::ResolveVariant: no shader assigned");
        return nullptr;
    }
    vector<std::string_view> enabled;
    enabled.reserve(_enabledKeywords.size());
    for (const string& kw : _enabledKeywords) {
        enabled.emplace_back(kw);
    }
    return _shader->GetOrCreateVariant(cache, passIndex, enabled, sm);
}

uint32_t MaterialAsset::ApplyProperties(render::ShaderParameterTable& table) const noexcept {
    uint32_t applied = 0;
    for (const auto& [name, value] : _properties) {
        const bool ok = std::visit(
            [&](auto&& v) -> bool {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>) {
                    return table.SetBytes(name, &v, sizeof(float));
                } else if constexpr (std::is_same_v<T, Eigen::Vector4f>) {
                    // Eigen::Vector4f 内存连续 (4 个 float)。
                    return table.SetBytes(name, v.data(), sizeof(float) * 4);
                } else if constexpr (std::is_same_v<T, render::TextureView*>) {
                    return v != nullptr && table.SetResource(name, static_cast<render::ResourceView*>(v));
                } else if constexpr (std::is_same_v<T, render::Sampler*>) {
                    return v != nullptr && table.SetSampler(name, v);
                } else {
                    return false;
                }
            },
            value);
        if (ok) {
            ++applied;
        }
    }
    return applied;
}

}  // namespace radray

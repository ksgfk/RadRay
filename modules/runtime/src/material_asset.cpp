#include <radray/runtime/material_asset.h>

#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <radray/runtime/render_framework/material_property_block.h>
#include <radray/runtime/render_framework/sampler_cache.h>

namespace radray {

MaterialAsset::MaterialAsset(StreamingAssetRef<ShaderAsset> shader) noexcept
    : _shader(std::move(shader)) {}

MaterialAsset::~MaterialAsset() noexcept = default;

void MaterialAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    // MaterialAsset 不拥有 GPU 资源 (texture view / sampler 由各自缓存持有:
    // texture view 归 TextureAsset, sampler 归 SamplerCache, 变体/参数表由各自缓存持有)。
    // property 只存 asset 引用 + 描述值。仅清空 CPU 状态。
    _properties.clear();
    _enabledKeywords.clear();
    _keywordIndex.clear();
    _shader.Reset();
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

void MaterialAsset::SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept {
    vector<byte> bytes(size);
    if (data != nullptr && size > 0) {
        std::memcpy(bytes.data(), data, size);
    }
    _properties[string{name}] = std::move(bytes);
}

void MaterialAsset::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept {
    _properties[string{name}] = std::move(texture);
}

void MaterialAsset::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture, const TextureSubViewDesc& sub) noexcept {
    _properties[string{name}] = TextureSubViewRef{std::move(texture), sub};
}

void MaterialAsset::SetSampler(std::string_view name, const render::SamplerDescriptor& desc) noexcept {
    _properties[string{name}] = desc;
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
    if (_shader.Get() == nullptr) {
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

namespace {

// 把一个 (name, value) property 展开进快照的对应列表。
// 常量 (float/vector/块) -> ConstantEntry (Name 可为块名或字段名, 绑定时由打包器解析)。
void EmitProperty(MaterialRenderSnapshot& snapshot, const string& name, const MaterialPropertyValue& value) noexcept {
    std::visit(
        [&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                MaterialRenderSnapshot::ConstantEntry e{};
                e.Name = name;
                e.Bytes.resize(sizeof(float));
                std::memcpy(e.Bytes.data(), &v, sizeof(float));
                snapshot.Constants.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, Eigen::Vector4f>) {
                MaterialRenderSnapshot::ConstantEntry e{};
                e.Name = name;
                e.Bytes.resize(sizeof(float) * 4);
                std::memcpy(e.Bytes.data(), v.data(), sizeof(float) * 4);
                snapshot.Constants.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, vector<byte>>) {
                if (!v.empty()) {
                    MaterialRenderSnapshot::ConstantEntry e{};
                    e.Name = name;
                    e.Bytes = v;
                    snapshot.Constants.emplace_back(std::move(e));
                }
            } else if constexpr (std::is_same_v<T, StreamingAssetRef<TextureAsset>>) {
                MaterialRenderSnapshot::TextureEntry e{};
                e.Name = name;
                e.Texture = v;
                snapshot.Textures.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, TextureSubViewRef>) {
                MaterialRenderSnapshot::TextureEntry e{};
                e.Name = name;
                e.Texture = v.Texture;
                e.SubView = v.SubView;
                snapshot.Textures.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, render::SamplerDescriptor>) {
                MaterialRenderSnapshot::SamplerEntry e{};
                e.Name = name;
                e.Desc = v;
                snapshot.Samplers.emplace_back(std::move(e));
            }
        },
        value);
}

}  // namespace

shared_ptr<const MaterialRenderSnapshot> MaterialAsset::CreateSnapshot() const noexcept {
    return CreateSnapshot(nullptr);
}

shared_ptr<const MaterialRenderSnapshot> MaterialAsset::CreateSnapshot(const MaterialPropertyBlock* overrides) const noexcept {
    auto snapshot = make_shared<MaterialRenderSnapshot>();
    snapshot->Shader = _shader;
    snapshot->EnabledKeywords = _enabledKeywords;
    snapshot->RenderQueue = _renderQueue;

    if (overrides == nullptr || overrides->IsEmpty()) {
        // 无覆盖: 直接铺模板 property。
        for (const auto& [name, value] : _properties) {
            EmitProperty(*snapshot, name, value);
        }
        return snapshot;
    }

    // 有覆盖: 合并模板 + 覆盖 (同名替换、新名追加)。一个名字被覆盖后类型可能改变
    // (如常量变纹理), 故先按名字合并成统一 map, 再统一 emit, 而非分别 append。
    unordered_map<string, const MaterialPropertyValue*> merged;
    merged.reserve(_properties.size() + overrides->GetOverrides().size());
    for (const auto& [name, value] : _properties) {
        merged[name] = &value;
    }
    for (const auto& [name, value] : overrides->GetOverrides()) {
        merged[name] = &value;  // 覆盖同名
    }
    for (const auto& [name, valuePtr] : merged) {
        EmitProperty(*snapshot, name, *valuePtr);
    }
    return snapshot;
}

uint32_t MaterialAsset::ApplyProperties(render::ShaderParameterTable& table, SamplerCache& samplerCache) const noexcept {
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
                } else if constexpr (std::is_same_v<T, vector<byte>>) {
                    return !v.empty() && table.SetBytes(name, v.data(), static_cast<uint32_t>(v.size()));
                } else if constexpr (std::is_same_v<T, StreamingAssetRef<TextureAsset>>) {
                    TextureAsset* tex = v.Get();
                    render::TextureView* srv = tex != nullptr ? tex->GetSrv() : nullptr;
                    return srv != nullptr && table.SetResource(name, static_cast<render::ResourceView*>(srv));
                } else if constexpr (std::is_same_v<T, TextureSubViewRef>) {
                    TextureAsset* tex = v.Texture.Get();
                    render::TextureView* srv = tex != nullptr ? tex->GetOrCreateSrv(v.SubView) : nullptr;
                    return srv != nullptr && table.SetResource(name, static_cast<render::ResourceView*>(srv));
                } else if constexpr (std::is_same_v<T, render::SamplerDescriptor>) {
                    render::Sampler* sampler = samplerCache.GetOrCreate(v).Get();
                    return sampler != nullptr && table.SetSampler(name, sampler);
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

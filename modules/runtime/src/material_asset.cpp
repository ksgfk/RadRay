#include <radray/runtime/material_asset.h>

#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <radray/hash.h>
#include <radray/runtime/render_framework/material_property_block.h>
#include <radray/runtime/sampler_cache.h>

namespace radray {

MaterialAsset::MaterialAsset(StreamingAssetRef<ShaderAsset> shader) noexcept
    : _shader(std::move(shader)) {}

MaterialAsset::~MaterialAsset() noexcept = default;

void MaterialAsset::SetShader(StreamingAssetRef<ShaderAsset> shader) noexcept {
    if (_shader.GetAssetId() == shader.GetAssetId() && _shader.GetHandle() == shader.GetHandle()) {
        return;
    }
    _shader = std::move(shader);
    ++_revision;
}

void MaterialAsset::SetRenderQueue(int32_t queue) noexcept {
    if (_renderQueue != queue) {
        _renderQueue = queue;
        ++_revision;
    }
}

void MaterialAsset::SetRenderState(const MaterialRenderState& state) noexcept {
    if (_renderState != state) {
        _renderState = state;
        ++_revision;
    }
}

void MaterialAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    // MaterialAsset 不拥有 GPU 资源 (texture view / sampler 由各自缓存持有:
    // texture view 归 TextureAsset, sampler 归 SamplerCache, 变体/参数表由各自缓存持有)。
    // property 只存 asset 引用 + 描述值。仅清空 CPU 状态。
    _properties.clear();
    _enabledKeywords.clear();
    _keywordIndex.clear();
    _shader.Reset();
    ++_revision;
}

AssetTypeId MaterialAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<MaterialAsset>;
}

void MaterialAsset::SetOverride(std::string_view name, MaterialPropertyValue value) noexcept {
    auto it = _properties.find(string{name});
    if (it != _properties.end() && MaterialPropertyValuesEqual(it->second, value)) {
        return;
    }
    _properties[string{name}] = std::move(value);
    ++_revision;
}

void MaterialAsset::SetFloat(std::string_view name, float value) noexcept {
    SetOverride(name, value);
}

void MaterialAsset::SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept {
    SetOverride(name, value);
}

void MaterialAsset::SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept {
    vector<byte> bytes(size);
    if (data != nullptr && size > 0) {
        std::memcpy(bytes.data(), data, size);
    }
    SetOverride(name, std::move(bytes));
}

void MaterialAsset::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept {
    SetOverride(name, std::move(texture));
}

void MaterialAsset::SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture, const TextureSubViewDesc& sub) noexcept {
    SetOverride(name, TextureSubViewRef{std::move(texture), sub});
}

void MaterialAsset::SetSampler(std::string_view name, const render::SamplerDescriptor& desc) noexcept {
    SetOverride(name, desc);
}

std::optional<MaterialPropertyValue> MaterialAsset::GetProperty(std::string_view name) const noexcept {
    if (auto value = GetOverride(name); value.has_value()) {
        return value;
    }
    ShaderAsset* shader = _shader.Get();
    const ShaderPropertyDesc* property = shader != nullptr ? shader->FindProperty(name).Get() : nullptr;
    return property != nullptr ? property->DefaultValue : std::nullopt;
}

std::optional<MaterialPropertyValue> MaterialAsset::GetOverride(std::string_view name) const noexcept {
    auto it = _properties.find(string{name});
    if (it == _properties.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool MaterialAsset::HasProperty(std::string_view name) const noexcept {
    if (_properties.find(string{name}) != _properties.end()) {
        return true;
    }
    ShaderAsset* shader = _shader.Get();
    return shader != nullptr && shader->FindProperty(name).HasValue();
}

void MaterialAsset::ClearOverride(std::string_view name) noexcept {
    auto it = _properties.find(string{name});
    if (it != _properties.end()) {
        _properties.erase(it);
        ++_revision;
    }
}

std::optional<float> MaterialAsset::GetFloat(std::string_view name) const noexcept {
    auto value = GetProperty(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (const float* v = std::get_if<float>(&*value)) {
        return *v;
    }
    return std::nullopt;
}

std::optional<Eigen::Vector4f> MaterialAsset::GetVector(std::string_view name) const noexcept {
    auto value = GetProperty(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (const Eigen::Vector4f* v = std::get_if<Eigen::Vector4f>(&*value)) {
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
    ++_revision;
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
    ++_revision;
}

bool MaterialAsset::IsKeywordEnabled(std::string_view name) const noexcept {
    return _keywordIndex.find(string{name}) != _keywordIndex.end();
}

Nullable<const CompiledShaderVariant*> MaterialAsset::ResolveVariant(
    ShaderVariantLibrary& cache,
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
void EmitProperty(
    MaterialRenderSnapshot::PropertyLayer& layer,
    const string& name,
    const MaterialPropertyValue& value) noexcept {
    std::visit(
        [&](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                MaterialRenderSnapshot::ConstantEntry e{};
                e.Name = name;
                e.Kind = ShaderPropertyKind::Float;
                e.Bytes.resize(sizeof(float));
                std::memcpy(e.Bytes.data(), &v, sizeof(float));
                layer.Constants.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, Eigen::Vector4f>) {
                MaterialRenderSnapshot::ConstantEntry e{};
                e.Name = name;
                e.Kind = ShaderPropertyKind::Vector;
                e.Bytes.resize(sizeof(float) * 4);
                std::memcpy(e.Bytes.data(), v.data(), sizeof(float) * 4);
                layer.Constants.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, vector<byte>>) {
                if (!v.empty()) {
                    MaterialRenderSnapshot::ConstantEntry e{};
                    e.Name = name;
                    e.Kind = ShaderPropertyKind::Bytes;
                    e.Bytes = v;
                    layer.Constants.emplace_back(std::move(e));
                }
            } else if constexpr (std::is_same_v<T, StreamingAssetRef<TextureAsset>>) {
                MaterialRenderSnapshot::TextureEntry e{};
                e.Name = name;
                e.Texture = v;
                layer.Textures.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, TextureSubViewRef>) {
                MaterialRenderSnapshot::TextureEntry e{};
                e.Name = name;
                e.Texture = v.Texture;
                e.SubView = v.SubView;
                layer.Textures.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, ShaderDefaultTexture>) {
                MaterialRenderSnapshot::TextureEntry e{};
                e.Name = name;
                e.DefaultTexture = v;
                layer.Textures.emplace_back(std::move(e));
            } else if constexpr (std::is_same_v<T, render::SamplerDescriptor>) {
                MaterialRenderSnapshot::SamplerEntry e{};
                e.Name = name;
                e.Desc = v;
                layer.Samplers.emplace_back(std::move(e));
            }
        },
        value);
}

MaterialBindingKey BuildMaterialBindingKey(const MaterialRenderSnapshot& snapshot) noexcept {
    vector<uint64_t> parts;
    auto appendBytes = [&parts](const void* data, size_t size) {
        parts.push_back(HashData64(data, size));
    };
    appendBytes(&snapshot.RenderQueue, sizeof(snapshot.RenderQueue));
    const AssetId& shaderId = snapshot.Shader.GetAssetId();
    appendBytes(&shaderId, sizeof(shaderId));
    const AssetHandle shaderHandle = snapshot.Shader.GetHandle();
    appendBytes(&shaderHandle, sizeof(shaderHandle));

    const auto& state = snapshot.RenderState;
    const uint32_t stateWords[] = {
        state.Cull.has_value() ? 1u : 0u,
        state.Cull.has_value() ? static_cast<uint32_t>(state.Cull.value()) : 0u,
        state.DepthWrite.has_value() ? 1u : 0u,
        state.DepthWrite.value_or(false) ? 1u : 0u,
        state.OverrideBlend ? 1u : 0u,
        state.Blend.has_value() ? 1u : 0u};
    appendBytes(stateWords, sizeof(stateWords));
    if (state.Blend.has_value()) {
        appendBytes(&state.Blend.value(), sizeof(state.Blend.value()));
    }
    for (const auto& keyword : snapshot.EnabledKeywords) {
        appendBytes(keyword.data(), keyword.size());
    }
    auto hashLayer = [&](const MaterialRenderSnapshot::PropertyLayer& layer, uint64_t marker) {
        vector<uint64_t> entries;
        for (const auto& constant : layer.Constants) {
            uint64_t entry = HashData64(constant.Name.data(), constant.Name.size());
            entry ^= static_cast<uint64_t>(constant.Kind) * 0x9e3779b97f4a7c15ull;
            entry ^= HashData64(constant.Bytes.data(), constant.Bytes.size()) + 0x9e3779b97f4a7c15ull;
            entries.push_back(entry);
        }
        for (const auto& texture : layer.Textures) {
            uint64_t entry = HashData64(texture.Name.data(), texture.Name.size());
            const AssetId& id = texture.Texture.GetAssetId();
            entry ^= HashData64(&id, sizeof(id));
            const AssetHandle handle = texture.Texture.GetHandle();
            entry ^= HashData64(&handle, sizeof(handle));
            const TextureViewKey view = BuildTextureViewKey(texture.SubView);
            entry ^= HashData64(&view, sizeof(view));
            if (texture.DefaultTexture.has_value()) {
                entry ^= HashData64(&*texture.DefaultTexture, sizeof(*texture.DefaultTexture));
            }
            entries.push_back(entry);
        }
        for (const auto& sampler : layer.Samplers) {
            uint64_t entry = HashData64(sampler.Name.data(), sampler.Name.size());
            const SamplerKey key = BuildSamplerKey(sampler.Desc);
            entry ^= HashData64(&key, sizeof(key));
            entries.push_back(entry);
        }
        std::ranges::sort(entries);
        uint64_t layerHash = marker;
        for (const uint64_t entry : entries) {
            layerHash ^= entry + 0x9e3779b97f4a7c15ull + (layerHash << 6u) + (layerHash >> 2u);
        }
        parts.push_back(layerHash);
    };
    hashLayer(snapshot.MaterialProperties, 0x4d4154455249414cull);
    hashLayer(snapshot.PropertyBlockProperties, 0x50524f50424c4f43ull);
    std::ranges::sort(parts);
    MaterialBindingKey key{.Lo = 0xcbf29ce484222325ull, .Hi = 0x84222325cbf29ce4ull};
    for (const uint64_t part : parts) {
        key.Lo ^= part + 0x9e3779b97f4a7c15ull + (key.Lo << 6u) + (key.Lo >> 2u);
        key.Hi += (part ^ (part >> 29u)) * 0x94d049bb133111ebull;
    }
    return key;
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
    snapshot->RenderState = _renderState;

    for (const auto& [name, value] : _properties) {
        EmitProperty(snapshot->MaterialProperties, name, value);
    }
    if (overrides != nullptr) {
        for (const auto& [name, value] : overrides->GetOverrides()) {
            EmitProperty(snapshot->PropertyBlockProperties, name, value);
        }
    }
    snapshot->BindingKey = BuildMaterialBindingKey(*snapshot);
    return snapshot;
}


}  // namespace radray

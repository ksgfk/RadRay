#include <radray/runtime/material.h>

#include <algorithm>
#include <radray/logger.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

bool PrepareMaterialVertexInput(const ShaderTargetProgramInterfaceDesc& interface, const VertexInput& meshInput, ResolvedVertexInputState& output) {
    return interface.Stages.HasFlag(render::ShaderStage::Vertex) && ResolveVertexInputState(meshInput, interface.VertexInputs, output);
}

std::optional<MaterialAsset> MaterialAsset::Create(const AssetManager& assets, const MaterialAssetDesc& desc) {
    unordered_set<string> names;
    for (const auto& parameter : desc.Parameters)
        if (parameter.Name.empty() || !names.insert(parameter.Name).second) {
            RADRAY_ERR_LOG("Empty or duplicate material parameter: {}", parameter.Name);
            return std::nullopt;
        }
    names.clear();
    vector<AssetHandle<ShaderAsset>> shaders;
    for (const auto& program : desc.Programs) {
        const auto shader = assets.Get(program.Shader);
        if (program.Name.empty() || !names.insert(program.Name).second || !shader || !shader->GetVariantInterface(program.VariantIndex)) return std::nullopt;
        const auto vs = shader->GetStage(program.VariantIndex, render::ShaderStage::Vertex);
        const auto ps = shader->GetStage(program.VariantIndex, render::ShaderStage::Pixel);
        if (!vs || program.VertexEntry != vs->EntryPoint || (!program.PixelEntry.empty() && (!ps || program.PixelEntry != ps->EntryPoint))) {
            RADRAY_ERR_LOG("Invalid material program entry: {}", program.Name);
            return std::nullopt;
        }
        if (std::find(shaders.begin(), shaders.end(), program.Shader) == shaders.end()) shaders.push_back(program.Shader);
    }
    if (shaders.size() != desc.Families.size()) return std::nullopt;
    MaterialAsset result;
    for (const auto& family : desc.Families) {
        if (std::find(shaders.begin(), shaders.end(), family.Shader) == shaders.end() ||
            std::any_of(result._bindings.begin(), result._bindings.end(), [&](const auto& b) { return b.Shader == family.Shader; })) return std::nullopt;
        const auto shader = assets.Get(family.Shader);
        if (!shader) return std::nullopt;
        auto map = MaterialParameterBindingMap::Create(desc.Parameters, *shader, family.Parameters, family.ConstantBlockName);
        if (!map) return std::nullopt;
        result._bindings.push_back({family.Shader, std::move(*map)});
    }
    result._properties = desc.Properties;
    result._programs = desc.Programs;
    result._parameters = desc.Parameters;
    return result;
}

const MaterialProperties& MaterialAsset::GetProperties() const noexcept { return _properties; }
std::span<const MaterialShaderProgram> MaterialAsset::GetPrograms() const noexcept { return _programs; }
std::optional<size_t> MaterialAsset::FindProgram(std::string_view name) const noexcept {
    for (size_t i = 0; i < _programs.size(); ++i)
        if (_programs[i].Name == name) return i;
    return std::nullopt;
}
Nullable<const MaterialShaderProgram*> MaterialAsset::GetProgram(size_t index) const noexcept { return index < _programs.size() ? &_programs[index] : nullptr; }
std::span<const MaterialParameter> MaterialAsset::GetParameters() const noexcept { return _parameters; }
std::optional<size_t> MaterialAsset::FindParameter(std::string_view name) const noexcept {
    for (size_t i = 0; i < _parameters.size(); ++i)
        if (_parameters[i].Name == name) return i;
    return std::nullopt;
}
Nullable<const MaterialParameterValue*> MaterialAsset::GetDefaultParameter(size_t index) const noexcept { return index < _parameters.size() ? &_parameters[index].DefaultValue : nullptr; }
Nullable<const MaterialParameterBindingMap*> MaterialAsset::GetBindingMap(AssetHandle<ShaderAsset> shader) const noexcept {
    for (const auto& binding : _bindings)
        if (binding.Shader == shader) return &binding.Map;
    return nullptr;
}

MaterialInstance::MaterialInstance(AssetHandle<MaterialAsset> material) noexcept : _material(material) {}
AssetHandle<MaterialAsset> MaterialInstance::GetMaterial() const noexcept { return _material; }
Nullable<const MaterialParameterValue*> MaterialInstance::GetParameter(const AssetManager& assets, size_t index) const noexcept {
    const auto material = assets.Get(_material);
    if (!material || index >= material->GetParameters().size()) return nullptr;
    return index < _overrides.size() && _overrides[index] ? &*_overrides[index] : material->GetDefaultParameter(index);
}
bool MaterialInstance::SetParameter(const AssetManager& assets, size_t index, MaterialParameterValue value) {
    const auto material = assets.Get(_material);
    const auto declared = material ? material->GetDefaultParameter(index) : nullptr;
    if (!declared || declared->index() != value.index()) return false;
    if (_overrides.size() < material->GetParameters().size()) _overrides.resize(material->GetParameters().size());
    _overrides[index] = std::move(value);
    return true;
}
bool MaterialInstance::ResetParameter(const AssetManager& assets, size_t index) noexcept {
    const auto material = assets.Get(_material);
    if (!material || index >= material->GetParameters().size()) return false;
    if (index < _overrides.size()) _overrides[index].reset();
    return true;
}

bool MaterialRenderProxy::Update(const AssetManager& assets, AssetHandle<MaterialAsset> handle) {
    const auto material = assets.Get(handle);
    if (!material) return false;
    vector<MaterialParameterValue> parameters;
    parameters.reserve(material->GetParameters().size());
    for (const auto& parameter : material->GetParameters()) parameters.push_back(parameter.DefaultValue);
    _parameters = std::move(parameters);
    _material = handle;
    return true;
}
bool MaterialRenderProxy::Update(const AssetManager& assets, const MaterialInstance& instance) {
    const auto material = assets.Get(instance.GetMaterial());
    if (!material) return false;
    vector<MaterialParameterValue> parameters;
    parameters.reserve(material->GetParameters().size());
    for (size_t i = 0; i < material->GetParameters().size(); ++i) parameters.push_back(*instance.GetParameter(assets, i));
    _parameters = std::move(parameters);
    _material = instance.GetMaterial();
    return true;
}
AssetHandle<MaterialAsset> MaterialRenderProxy::GetMaterial() const noexcept { return _material; }
Nullable<const MaterialParameterValue*> MaterialRenderProxy::GetParameter(size_t index) const noexcept { return index < _parameters.size() ? &_parameters[index] : nullptr; }

}  // namespace radray

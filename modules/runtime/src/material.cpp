#include <radray/runtime/material.h>

#include <algorithm>
#include <utility>

#include <radray/runtime/shader_program.h>

namespace radray {

struct Material::ResourceState {
    struct TextureValue {
        string Name;
        ShaderParameterInfo Parameter;
        uint32_t Element{0};
        StreamingAssetRef<TextureAsset> Texture;
        TextureSubViewDesc SubView;
    };

    struct SamplerValue {
        string Name;
        ShaderParameterInfo Parameter;
        uint32_t Element{0};
        render::SamplerDescriptor Sampler;
    };

    vector<TextureValue> Textures;
    vector<SamplerValue> Samplers;
};

Nullable<unique_ptr<Material>> Material::Create(const MaterialTechnique* technique) {
    return unique_ptr<Material>{new Material(technique)};
}

Material::Material(const MaterialTechnique* technique)
    : _technique(technique),
      _program(technique->GetPrimaryPass().Program),
      _parameterGroup(*technique->GetPrimaryPass().ParameterGroup),
      _parameters(&_program->GetParameterLayout(), _parameterGroup),
      _resources(make_unique<ResourceState>()) {
    for (const auto& pass : technique->Passes()) _pipelineStates.push_back(pass.DefaultPipelineState);
}

Material::~Material() noexcept = default;

string Material::CanonicalName(std::string_view name) const {
    const string prefix = _technique->GetPrimaryPass().MaterialBufferAnchor + ".";
    return name.starts_with(prefix) ? string{name} : prefix + string{name};
}

bool Material::SetPassPipelineState(std::string_view pass, const MaterialPipelineState& state) noexcept {
    const auto layout = _technique->FindPass(pass);
    if (!layout) return false;
    _pipelineStates[layout.Get() - _technique->Passes().data()] = state;
    return true;
}

Nullable<const MaterialPassRenderData*> MaterialRenderData::FindPass(std::string_view name) const noexcept {
    for (const auto& pass : Passes)
        if (pass.PassName == name) return &pass;
    return nullptr;
}

Nullable<const ShaderParameterInfo*> Material::FindNumericParameter(
    std::string_view name,
    ShaderParameterKind kind) const noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(CanonicalName(name));
    if (parameter == nullptr || parameter->Kind != kind ||
        parameter->Group != _parameterGroup) {
        return nullptr;
    }
    return parameter;
}

bool Material::SetFloat(std::string_view name, float value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetFloat(CanonicalName(name), value, element);
}

bool Material::SetFloat2(
    std::string_view name, const Eigen::Vector2f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat2(CanonicalName(name), value, element);
}

bool Material::SetFloat3(
    std::string_view name, const Eigen::Vector3f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat3(CanonicalName(name), value, element);
}

bool Material::SetFloat4(
    std::string_view name, const Eigen::Vector4f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat4(CanonicalName(name), value, element);
}

bool Material::SetInt(std::string_view name, int32_t value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetInt(CanonicalName(name), value, element);
}

bool Material::SetUInt(std::string_view name, uint32_t value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetUInt(CanonicalName(name), value, element);
}

bool Material::SetMatrix4x4(
    std::string_view name, const Eigen::Matrix4f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Matrix) != nullptr &&
           _parameters.SetMatrix4x4(CanonicalName(name), value, element);
}

bool Material::SetTexture(
    std::string_view name,
    StreamingAssetRef<TextureAsset> texture,
    const TextureSubViewDesc& subView,
    uint32_t element) noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(name);
    if (parameter == nullptr || parameter->Kind != ShaderParameterKind::Texture ||
        parameter->Group != _parameterGroup ||
        element >= parameter->ElementCount || !texture.IsValid()) {
        return false;
    }
    auto found = std::find_if(
        _resources->Textures.begin(),
        _resources->Textures.end(),
        [name, element](const ResourceState::TextureValue& value) noexcept {
            return value.Name == name && value.Element == element;
        });
    if (found != _resources->Textures.end()) {
        if (found->Texture == texture && found->SubView == subView) {
            return true;
        }
        found->Texture = std::move(texture);
        found->SubView = subView;
    } else {
        _resources->Textures.push_back(ResourceState::TextureValue{
            .Name = string{name},
            .Parameter = *parameter,
            .Element = element,
            .Texture = std::move(texture),
            .SubView = subView});
    }
    return true;
}

bool Material::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler,
    uint32_t element) noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(name);
    if (parameter == nullptr || parameter->Kind != ShaderParameterKind::Sampler ||
        parameter->Group != _parameterGroup ||
        element >= parameter->ElementCount) {
        return false;
    }
    auto found = std::find_if(
        _resources->Samplers.begin(),
        _resources->Samplers.end(),
        [name, element](const ResourceState::SamplerValue& value) noexcept {
            return value.Name == name && value.Element == element;
        });
    if (found != _resources->Samplers.end()) {
        if (found->Sampler == sampler) {
            return true;
        }
        found->Sampler = sampler;
    } else {
        _resources->Samplers.push_back(ResourceState::SamplerValue{
            .Name = string{name},
            .Parameter = *parameter,
            .Element = element,
            .Sampler = sampler});
    }
    return true;
}

bool Material::BuildRenderData(MaterialRenderData& out, vector<StreamingAssetRefAny>& retainedAssets) const {
    MaterialRenderData snapshot;
    snapshot.Queue = _renderQueue;
    vector<StreamingAssetRefAny> owners;
    const auto canonicalBytes = _parameters.GetBufferData(*_technique->GetPrimaryPass().BufferIndex);
    bool anyValid = false;
    for (uint32_t index = 0; index < _technique->Passes().size(); ++index) {
        const auto& layout = _technique->Passes()[index];
        MaterialPassRenderData pass;
        pass.PassName = layout.Name;
        pass.Program = layout.Program;
        pass.ParameterGroup = layout.ParameterGroup;
        pass.PipelineState = _pipelineStates[index];
        pass.Valid = true;
        if (layout.BufferIndex) {
            pass.Parameters = ShaderParameterStorage{&layout.Program->GetParameterLayout(), layout.ParameterGroup};
            pass.Valid = pass.Parameters.CopyCompatibleBufferBytes(*layout.BufferIndex, canonicalBytes);
        }
        for (const auto& resource : layout.Resources) {
            for (uint32_t element = 0; element < resource.Info.ElementCount; ++element) {
                if (resource.Info.Kind == ShaderParameterKind::Texture) {
                    const auto value = std::find_if(_resources->Textures.begin(), _resources->Textures.end(), [&](const auto& entry) {
                        return entry.Name == resource.Name && entry.Element == element;
                    });
                    const auto texture = value != _resources->Textures.end() ? value->Texture.Get() : nullptr;
                    if (!texture) {
                        pass.Valid = false;
                        continue;
                    }
                    pass.Textures.push_back({resource.Info, texture.Get(), value->SubView, element});
                    owners.push_back(value->Texture.AsAny());
                } else {
                    const auto value = std::find_if(_resources->Samplers.begin(), _resources->Samplers.end(), [&](const auto& entry) {
                        return entry.Name == resource.Name && entry.Element == element;
                    });
                    if (value == _resources->Samplers.end()) {
                        pass.Valid = false;
                        continue;
                    }
                    pass.Samplers.push_back({resource.Info, value->Sampler, element});
                }
            }
        }
        anyValid |= pass.Valid;
        snapshot.Passes.push_back(std::move(pass));
    }
    if (anyValid) retainedAssets.insert(retainedAssets.end(), std::make_move_iterator(owners.begin()), std::make_move_iterator(owners.end()));
    out = std::move(snapshot);
    return anyValid;
}

}  // namespace radray

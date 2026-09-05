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

Nullable<unique_ptr<Material>> Material::Create(
    ShaderProgram* program,
    std::string_view parameterGroupAnchor) {
    const auto buffers = program->GetParameterLayout().Buffers();
    const auto anchor = std::find_if(buffers.begin(), buffers.end(),
                                     [parameterGroupAnchor](const ShaderParameterBufferLayout& buffer) {
                                         return buffer.Name == parameterGroupAnchor;
                                     });
    if (anchor == buffers.end()) {
        return nullptr;
    }
    return unique_ptr<Material>{new Material(program, anchor->Group)};
}

Material::Material(ShaderProgram* program, uint32_t parameterGroup)
    : _program(program),
      _parameterGroup(parameterGroup),
      _parameters(&program->GetParameterLayout(), parameterGroup),
      _resources(make_unique<ResourceState>()) {}

Material::~Material() noexcept = default;

Nullable<const ShaderParameterInfo*> Material::FindNumericParameter(
    std::string_view name,
    ShaderParameterKind kind) const noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(name);
    if (parameter == nullptr || parameter->Kind != kind ||
        parameter->Group != _parameterGroup) {
        return nullptr;
    }
    return parameter;
}

bool Material::SetFloat(std::string_view name, float value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetFloat(name, value, element);
}

bool Material::SetFloat2(
    std::string_view name, const Eigen::Vector2f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat2(name, value, element);
}

bool Material::SetFloat3(
    std::string_view name, const Eigen::Vector3f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat3(name, value, element);
}

bool Material::SetFloat4(
    std::string_view name, const Eigen::Vector4f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Vector) != nullptr &&
           _parameters.SetFloat4(name, value, element);
}

bool Material::SetInt(std::string_view name, int32_t value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetInt(name, value, element);
}

bool Material::SetUInt(std::string_view name, uint32_t value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Scalar) != nullptr &&
           _parameters.SetUInt(name, value, element);
}

bool Material::SetMatrix4x4(
    std::string_view name, const Eigen::Matrix4f& value, uint32_t element) noexcept {
    return FindNumericParameter(name, ShaderParameterKind::Matrix) != nullptr &&
           _parameters.SetMatrix4x4(name, value, element);
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

bool Material::BuildRenderData(
    MaterialRenderData& out,
    vector<StreamingAssetRefAny>& retainedAssets) const {
    // Fail before publishing an incomplete snapshot or retaining partial resources.
    for (const ShaderParameterRecord& parameter : _program->GetParameterLayout().Parameters()) {
        if (parameter.Info.Group != _parameterGroup) {
            continue;
        }
        for (uint32_t element = 0; element < parameter.Info.ElementCount; ++element) {
            if (parameter.Info.Kind == ShaderParameterKind::Texture) {
                const auto found = std::find_if(_resources->Textures.begin(), _resources->Textures.end(),
                                                [&](const ResourceState::TextureValue& value) {
                                                    return value.Parameter.Binding == parameter.Info.Binding &&
                                                           value.Element == element;
                                                });
                if (found == _resources->Textures.end() || found->Texture.Get() == nullptr) {
                    return false;
                }
            } else if (parameter.Info.Kind == ShaderParameterKind::Sampler) {
                const auto found = std::find_if(_resources->Samplers.begin(), _resources->Samplers.end(),
                                                [&](const ResourceState::SamplerValue& value) {
                                                    return value.Parameter.Binding == parameter.Info.Binding &&
                                                           value.Element == element;
                                                });
                if (found == _resources->Samplers.end()) {
                    return false;
                }
            }
        }
    }
    MaterialRenderData snapshot{
        .Program = _program,
        .ParameterGroup = _parameterGroup,
        .Parameters = _parameters,
        .PipelineState = _pipelineState,
        .Queue = _renderQueue};
    for (const ResourceState::TextureValue& value : _resources->Textures) {
        snapshot.Textures.push_back(MaterialTextureFrameData{
            .Parameter = value.Parameter,
            .Texture = value.Texture.Get().Get(),
            .SubView = value.SubView,
            .Element = value.Element});
        retainedAssets.push_back(value.Texture.AsAny());
    }
    for (const ResourceState::SamplerValue& value : _resources->Samplers) {
        snapshot.Samplers.push_back(MaterialSamplerFrameData{
            .Parameter = value.Parameter,
            .Sampler = value.Sampler,
            .Element = value.Element});
    }
    out = std::move(snapshot);
    return true;
}

}  // namespace radray

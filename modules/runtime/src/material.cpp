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

    struct FlightSet {
        unique_ptr<render::ShaderParameterSet> Set;
        uint64_t ResourceVersion{0};
        vector<MaterialBufferBinding> BufferBindings;
    };

    explicit ResourceState(uint32_t flightCount)
        : Flights(flightCount) {}

    vector<TextureValue> Textures;
    vector<SamplerValue> Samplers;
    vector<FlightSet> Flights;
    uint64_t Version{1};
};

Nullable<unique_ptr<Material>> Material::Create(
    ShaderProgram* program,
    BindingGroupPlan bindingGroups,
    uint32_t flightCount) {
    if (program == nullptr || !bindingGroups.IsValid() || flightCount == 0) {
        return nullptr;
    }
    const bool materialGroupExists = std::any_of(
        program->GetArtifact().Generic().Bindings().begin(),
        program->GetArtifact().Generic().Bindings().end(),
        [group = bindingGroups.MaterialGroup](const shader::WireBindingRecord& binding) noexcept {
            return binding.Group == group;
        });
    if (!materialGroupExists) {
        return nullptr;
    }
    return unique_ptr<Material>{new Material(program, bindingGroups, flightCount)};
}

Material::Material(
    ShaderProgram* program,
    BindingGroupPlan bindingGroups,
    uint32_t flightCount)
    : _program(program),
      _bindingGroups(bindingGroups),
      _parameters(&program->GetParameterLayout()),
      _resources(make_unique<ResourceState>(flightCount)) {}

Material::~Material() noexcept = default;

const ShaderParameterInfo* Material::FindNumericParameter(
    std::string_view name,
    ShaderParameterKind kind) const noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(name);
    if (parameter == nullptr || parameter->Kind != kind ||
        parameter->Group != _bindingGroups.MaterialGroup) {
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
        parameter->Group != _bindingGroups.MaterialGroup ||
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
    ++_resources->Version;
    return true;
}

bool Material::SetSampler(
    std::string_view name,
    const render::SamplerDescriptor& sampler,
    uint32_t element) noexcept {
    const ShaderParameterInfo* parameter = _program->GetParameterLayout().Find(name);
    if (parameter == nullptr || parameter->Kind != ShaderParameterKind::Sampler ||
        parameter->Group != _bindingGroups.MaterialGroup ||
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
    ++_resources->Version;
    return true;
}

Nullable<render::ShaderParameterSet*> Material::PrepareParameterSet(
    uint32_t flightIndex,
    std::span<const MaterialBufferBinding> bufferBindings) noexcept {
    if (flightIndex >= _resources->Flights.size()) {
        return nullptr;
    }

    vector<const MaterialBufferBinding*> materialBuffers;
    for (uint32_t bufferIndex = 0;
         bufferIndex < _program->GetParameterLayout().Buffers().size();
         ++bufferIndex) {
        const ShaderParameterBufferLayout& buffer =
            _program->GetParameterLayout().Buffers()[bufferIndex];
        if (buffer.Group != _bindingGroups.MaterialGroup) {
            continue;
        }
        const auto found = std::find_if(
            bufferBindings.begin(),
            bufferBindings.end(),
            [bufferIndex](const MaterialBufferBinding& value) noexcept {
                return value.BufferIndex == bufferIndex;
            });
        if (found == bufferBindings.end() || found->Value.Target == nullptr) {
            return nullptr;
        }
        materialBuffers.push_back(&*found);
    }
    if (materialBuffers.size() != bufferBindings.size()) {
        return nullptr;
    }

    for (const ShaderParameterRecord& parameter : _program->GetParameterLayout().Parameters()) {
        if (parameter.Info.Group != _bindingGroups.MaterialGroup ||
            (parameter.Info.Kind != ShaderParameterKind::Texture &&
             parameter.Info.Kind != ShaderParameterKind::Sampler)) {
            continue;
        }
        for (uint32_t element = 0; element < parameter.Info.ElementCount; ++element) {
            const bool exists = parameter.Info.Kind == ShaderParameterKind::Texture
                                    ? std::any_of(
                                          _resources->Textures.begin(),
                                          _resources->Textures.end(),
                                          [&](const ResourceState::TextureValue& value) noexcept {
                                              return value.Name == parameter.Name &&
                                                     value.Element == element;
                                          })
                                    : std::any_of(
                                          _resources->Samplers.begin(),
                                          _resources->Samplers.end(),
                                          [&](const ResourceState::SamplerValue& value) noexcept {
                                              return value.Name == parameter.Name &&
                                                     value.Element == element;
                                          });
            if (!exists) {
                return nullptr;
            }
        }
    }

    ResourceState::FlightSet& flight = _resources->Flights[flightIndex];
    const bool rebuild = flight.Set == nullptr ||
                         flight.ResourceVersion != _resources->Version;
    // A resident set may already be referenced by command buffers recorded for an
    // earlier camera this frame. Vulkan resolves descriptors at execution time, so
    // rewriting an unchanged binding would retroactively repoint that recording.
    bool bufferBindingsChanged = rebuild ||
                                 flight.BufferBindings.size() != materialBuffers.size();
    if (!bufferBindingsChanged) {
        for (size_t index = 0; index < materialBuffers.size(); ++index) {
            if (flight.BufferBindings[index].BufferIndex !=
                    materialBuffers[index]->BufferIndex ||
                !(flight.BufferBindings[index].Value == materialBuffers[index]->Value)) {
                bufferBindingsChanged = true;
                break;
            }
        }
    }
    if (!bufferBindingsChanged) {
        return flight.Set.get();
    }
    if (!rebuild) {
        // Rewriting a live set is only safe before anything binds it this frame.
        // Callers that prepare the same material twice per frame with different
        // targets (for example one camera per window, once the arena spills into a
        // new block) would repoint an earlier recording under Vulkan.
        RADRAY_WARN_LOG(
            "material parameter set rewritten with new buffer targets on flight {}; "
            "earlier recordings this frame may observe the new binding",
            flightIndex);
    }
    unique_ptr<render::ShaderParameterSet> replacement;
    render::ShaderParameterSet* set = flight.Set.get();
    if (rebuild) {
        Nullable<unique_ptr<render::ShaderParameterSet>> created =
            _program->GetDevice()->CreateShaderParameterSet(
                render::ShaderParameterSetDescriptor{
                    .Layout = _program->GetPipelineLayout(),
                    .GroupIndex = _bindingGroups.MaterialGroup});
        if (!created.HasValue()) {
            return nullptr;
        }
        replacement = created.Release();
        set = replacement.get();
    }

    for (const MaterialBufferBinding* binding : materialBuffers) {
        const ShaderParameterBufferLayout& buffer =
            _program->GetParameterLayout().Buffers()[binding->BufferIndex];
        if (!set->Set(buffer.Binding, 0, binding->Value)) {
            return nullptr;
        }
    }
    if (rebuild) {
        for (const ResourceState::TextureValue& value : _resources->Textures) {
            TextureAsset* texture = value.Texture.Get();
            render::TextureView* view =
                texture != nullptr ? texture->GetOrCreateSrv(value.SubView) : nullptr;
            if (view == nullptr || !set->Set(value.Parameter.Binding, value.Element, view)) {
                return nullptr;
            }
        }
        for (const ResourceState::SamplerValue& value : _resources->Samplers) {
            const Nullable<render::Sampler*> sampler =
                _program->GetDevice()->GetOrCreateSampler(value.Sampler);
            if (!sampler.HasValue() ||
                !set->Set(value.Parameter.Binding, value.Element, sampler.Get())) {
                return nullptr;
            }
        }
    }
    if (!set->FlushWrites()) {
        return nullptr;
    }
    if (rebuild) {
        flight.Set = std::move(replacement);
        flight.ResourceVersion = _resources->Version;
    }
    flight.BufferBindings.clear();
    flight.BufferBindings.reserve(materialBuffers.size());
    for (const MaterialBufferBinding* binding : materialBuffers) {
        flight.BufferBindings.push_back(*binding);
    }
    return flight.Set.get();
}

Nullable<render::ShaderParameterSet*> Material::GetResidentParameterSet(
    uint32_t flightIndex) const noexcept {
    if (flightIndex >= _resources->Flights.size()) {
        return nullptr;
    }
    return _resources->Flights[flightIndex].Set.get();
}

uint64_t Material::GetResourceVersion() const noexcept {
    return _resources->Version;
}

uint64_t Material::GetResidentResourceVersion(uint32_t flightIndex) const noexcept {
    return flightIndex < _resources->Flights.size()
               ? _resources->Flights[flightIndex].ResourceVersion
               : 0;
}

}  // namespace radray

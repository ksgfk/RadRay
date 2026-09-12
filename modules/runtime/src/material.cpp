#include <radray/runtime/material.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include <radray/runtime/shader_program.h>

namespace radray {

namespace {
std::atomic<uint64_t> gNextMaterialGeneration{1};

template <typename T>
std::span<const byte> AsBytes(const T& value) noexcept {
    return std::as_bytes(std::span{&value, size_t{1}});
}
}  // namespace

struct Material::ResourceState {
    struct TextureValue {
        string Name;
        ShaderParameterInfo Parameter;
        uint32_t Element{0};
        StreamingAssetRef<TextureAsset> Texture;
        TextureSubViewDesc SubView;
        Nullable<TextureAsset*> ObservedTexture{nullptr};
    };

    struct SamplerValue {
        string Name;
        ShaderParameterInfo Parameter;
        uint32_t Element{0};
        render::SamplerDescriptor Sampler;
    };

    vector<TextureValue> Textures;
    vector<SamplerValue> Samplers;
    vector<uint32_t> PendingTextures;
};

Nullable<unique_ptr<Material>> Material::Create(const MaterialTechnique* technique) {
    return unique_ptr<Material>{new Material(technique)};
}

Material::Material(const MaterialTechnique* technique)
    : _technique(technique),
      _program(technique->GetPrimaryPass().Program),
      _parameterGroup(*technique->GetPrimaryPass().ParameterGroup),
      _numericBytes(
          technique->GetPrimaryPass().BufferIndex
              ? technique->GetPrimaryPass().Program->GetParameterLayout().Buffers()[*technique->GetPrimaryPass().BufferIndex].Size
              : 0),
      _parameters(&_program->GetParameterLayout(), _parameterGroup, _numericBytes),
      _generation(gNextMaterialGeneration.fetch_add(1, std::memory_order_relaxed)),
      _resources(make_unique<ResourceState>()) {
    if (_generation == 0 || _generation == UINT64_MAX) RADRAY_ABORT("Material generation exhausted");
    for (const auto& pass : technique->Passes()) _pipelineStates.push_back(pass.DefaultPipelineState);
    _observedPipelineStates = _pipelineStates;
    _observedNumericBytes = _numericBytes;
}

Material::~Material() noexcept {
    NotifyChanged(MaterialDirtyKind::Removed);
}

void Material::AddChangeListener(void* context, ChangeCallback callback) {
    const auto found = std::find_if(_changeListeners.begin(), _changeListeners.end(),
                                    [context](const auto& listener) { return listener.Context == context; });
    if (found == _changeListeners.end()) _changeListeners.push_back({context, callback});
}

void Material::RemoveChangeListener(void* context) noexcept {
    std::erase_if(_changeListeners, [context](const auto& listener) { return listener.Context == context; });
}

void Material::NotifyChanged(MaterialDirtyFlags flags) const noexcept {
    for (const auto& listener : _changeListeners) listener.Callback(listener.Context, const_cast<Material&>(*this), flags);
}

void Material::MarkEscaped(bool& flag) noexcept {
    if (flag) return;
    flag = true;
    NotifyChanged(MaterialDirtyKind::Observe);
}

void Material::RetainAssetReferences(vector<StreamingAssetRefAny>& out) const {
    for (const auto& value : _resources->Textures)
        if (value.ObservedTexture) out.push_back(value.Texture.AsAny());
}

void Material::MarkChanged(ChangeKind kind, bool tracked) const noexcept {
    if (_revisions.Revision == UINT64_MAX) RADRAY_ABORT("Material revision exhausted");
    ++_revisions.Revision;
    switch (kind) {
        case ChangeKind::Values:
            ++_revisions.ValuesRevision;
            NotifyChanged(MaterialDirtyKind::Values);
            break;
        case ChangeKind::Bindings:
            ++_revisions.BindingsRevision;
            NotifyChanged(MaterialDirtyKind::Bindings);
            break;
        case ChangeKind::Structure:
            ++_revisions.StructureRevision;
            NotifyChanged(MaterialDirtyKind::Structure);
            break;
    }
    if (tracked) ++_observationStats.TrackedChanges;
}

void Material::ObserveChanges() const noexcept {
    if (HasEscapedWrites()) ++_observationStats.LegacyMaterialsObserved;
    if (_pipelineStateEscaped) {
        _observationStats.LegacyBytesCompared += _pipelineStates.size() * sizeof(MaterialPipelineState);
        if (_observedPipelineStates != _pipelineStates) {
            _observedPipelineStates = _pipelineStates;
            MarkChanged(ChangeKind::Structure, false);
        }
    }
    if (_numericEscaped) {
        _observationStats.LegacyBytesCompared += _numericBytes.size();
        if (_observedNumericBytes != _numericBytes) {
            _observedNumericBytes = _numericBytes;
            MarkChanged(ChangeKind::Values, false);
        }
    }
    auto& pending = _resources->PendingTextures;
    for (size_t index = 0; index < pending.size();) {
        auto& value = _resources->Textures[pending[index]];
        ++_observationStats.PendingResourcesObserved;
        const auto ready = value.Texture.Get();
        if (ready != value.ObservedTexture) {
            value.ObservedTexture = ready;
            MarkChanged(ChangeKind::Bindings, false);
        }
        if (value.Texture.IsCompleted()) {
            pending[index] = pending.back();
            pending.pop_back();
        } else {
            ++index;
        }
    }
}

uint64_t Material::GetRevision() const noexcept {
    ObserveChanges();
    return _revisions.Revision;
}

MaterialRevisions Material::GetRevisions(uint64_t epoch) const noexcept {
    if (_observedEpoch != epoch) {
        ObserveChanges();
        _observedEpoch = epoch;
        _epochRevisions = _revisions;
    }
    return _epochRevisions;
}

bool Material::HasPendingResources() const noexcept {
    return !_resources->PendingTextures.empty();
}

void Material::SetRenderQueue(RenderQueue value) noexcept {
    if (_renderQueue == value) return;
    _renderQueue = value;
    MarkChanged(ChangeKind::Structure);
}

string Material::CanonicalName(std::string_view name) const {
    const string prefix = _technique->GetPrimaryPass().MaterialBufferAnchor + ".";
    return name.starts_with(prefix) ? string{name} : prefix + string{name};
}

bool Material::SetPassPipelineState(std::string_view pass, const MaterialPipelineState& state) noexcept {
    const auto layout = _technique->FindPass(pass);
    if (!layout) return false;
    const auto index = layout.Get() - _technique->Passes().data();
    if (_pipelineStates[index] == state) return true;
    _pipelineStates[index] = state;
    _observedPipelineStates[index] = state;
    MarkChanged(ChangeKind::Structure);
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
    return SetNumericBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool Material::SetFloat2(
    std::string_view name, const Eigen::Vector2f& value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool Material::SetFloat3(
    std::string_view name, const Eigen::Vector3f& value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool Material::SetFloat4(
    std::string_view name, const Eigen::Vector4f& value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool Material::SetInt(std::string_view name, int32_t value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool Material::SetUInt(std::string_view name, uint32_t value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool Material::SetMatrix4x4(
    std::string_view name, const Eigen::Matrix4f& value, uint32_t element) noexcept {
    return SetNumericBytes(name, ShaderParameterKind::Matrix, AsBytes(value), element);
}

bool Material::SetNumericBytes(std::string_view name, ShaderParameterKind kind,
                               std::span<const byte> value, uint32_t element) noexcept {
    const auto parameter = FindNumericParameter(name, kind);
    if (!parameter || parameter->Size != value.size() || element >= parameter->ElementCount) return false;
    const auto bytes = _parameters.GetBufferData(parameter->BufferIndex);
    const uint64_t offset = uint64_t{parameter->ByteOffset} + uint64_t{element} * parameter->Stride;
    if (offset > bytes.size() || value.size() > bytes.size() - offset) return false;
    if (std::memcmp(bytes.data() + offset, value.data(), value.size()) == 0) return true;
    if (!_parameters.SetBytes(*parameter, kind, value, element)) return false;
    std::memcpy(_observedNumericBytes.data() + offset, value.data(), value.size());
    MarkChanged(ChangeKind::Values);
    return true;
}

bool Material::SetNumericData(std::span<const byte> value) noexcept {
    if (value.size() != _numericBytes.size()) return false;
    if (std::equal(value.begin(), value.end(), _numericBytes.begin())) return true;
    std::copy(value.begin(), value.end(), _numericBytes.begin());
    _observedNumericBytes = _numericBytes;
    MarkChanged(ChangeKind::Values);
    return true;
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
        found = _resources->Textures.end() - 1;
    }
    found->ObservedTexture = found->Texture.Get();
    const auto index = static_cast<uint32_t>(found - _resources->Textures.begin());
    std::erase(_resources->PendingTextures, index);
    if (!found->Texture.IsCompleted()) _resources->PendingTextures.push_back(index);
    MarkChanged(ChangeKind::Bindings);
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
    MarkChanged(ChangeKind::Bindings);
    return true;
}

bool Material::BuildRenderData(MaterialRenderData& out, vector<StreamingAssetRefAny>& retainedAssets,
                               Nullable<uint64_t*> bytesCopied,
                               std::optional<uint64_t> observationEpoch) const {
    if (bytesCopied != nullptr) *bytesCopied = 0;
    if (observationEpoch)
        GetRevisions(*observationEpoch);
    else
        ObserveChanges();
    const auto revisions = observationEpoch ? _epochRevisions : _revisions;
    if (revisions.Revision != _revisions.Revision) return false;
    const uint64_t revision = revisions.Revision;
    const auto retainReadyTextures = [&] {
        for (const auto& value : _resources->Textures)
            if (value.ObservedTexture) retainedAssets.push_back(value.Texture.AsAny());
    };
    if (out.Generation == _generation && out.Revision == revision &&
        out.Passes.size() == _technique->Passes().size()) {
        const bool valid = std::any_of(out.Passes.begin(), out.Passes.end(), [](const auto& pass) { return pass.Valid; });
        if (valid) retainReadyTextures();
        return valid;
    }
    auto& snapshot = out;
    snapshot.Queue = _renderQueue;
    snapshot.Passes.resize(_technique->Passes().size());
    bool anyValid = false;
    for (uint32_t index = 0; index < _technique->Passes().size(); ++index) {
        const auto& layout = _technique->Passes()[index];
        auto& pass = snapshot.Passes[index];
        pass.Textures.clear();
        pass.Samplers.clear();
        pass.PassName = layout.Name;
        pass.Program = layout.Program;
        pass.ProgramGeneration = layout.Program->GetGeneration();
        pass.ParameterGroup = layout.ParameterGroup;
        pass.PipelineState = _pipelineStates[index];
        pass.Valid = true;
        if (layout.BufferIndex) {
            // The snapshot bytes are the GPU cbuffer contents verbatim, so they only describe this
            // pass when they are exactly the anchored buffer's size and that buffer is in the group.
            const auto& buffer = layout.Program->GetParameterLayout().Buffers()[*layout.BufferIndex];
            pass.NumericBytes.assign(_numericBytes.begin(), _numericBytes.end());
            pass.Valid = !_numericBytes.empty() && _numericBytes.size() == buffer.Size &&
                         (!layout.ParameterGroup || buffer.Group == *layout.ParameterGroup);
            if (bytesCopied != nullptr && pass.Valid) *bytesCopied += _numericBytes.size();
        } else {
            pass.NumericBytes.clear();
        }
        for (const auto& resource : layout.Resources) {
            for (uint32_t element = 0; element < resource.Info.ElementCount; ++element) {
                if (resource.Info.Kind == ShaderParameterKind::Texture) {
                    const auto value = std::find_if(_resources->Textures.begin(), _resources->Textures.end(), [&](const auto& entry) {
                        return entry.Name == resource.Name && entry.Element == element;
                    });
                    const auto texture = value != _resources->Textures.end() ? value->ObservedTexture : nullptr;
                    if (!texture) {
                        pass.Valid = false;
                        continue;
                    }
                    pass.Textures.push_back({resource.Info, texture.Get(), value->SubView, element});
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
    }
    snapshot.Generation = _generation;
    snapshot.Revision = revision;
    snapshot.StructureRevision = revisions.StructureRevision;
    snapshot.ValuesRevision = revisions.ValuesRevision;
    snapshot.BindingsRevision = revisions.BindingsRevision;
    if (anyValid) retainReadyTextures();
    return anyValid;
}

}  // namespace radray

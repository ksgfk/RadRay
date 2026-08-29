#include <radray/runtime/shader_parameters.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include <radray/logger.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray {
namespace {

constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

std::optional<ShaderParameterKind> GetLeafKind(uint32_t kind) noexcept {
    switch (static_cast<shader::ShaderTypeKind>(kind)) {
        case shader::ShaderTypeKind::Scalar: return ShaderParameterKind::Scalar;
        case shader::ShaderTypeKind::Vector: return ShaderParameterKind::Vector;
        case shader::ShaderTypeKind::Matrix: return ShaderParameterKind::Matrix;
        case shader::ShaderTypeKind::Struct:
        case shader::ShaderTypeKind::Array:
        case shader::ShaderTypeKind::Member:
            return std::nullopt;
    }
    return std::nullopt;
}

struct ArrayContext {
    uint32_t Count{1};
    uint32_t Stride{0};
};

bool BuildBufferParameters(
    ShaderParameterLayout& result,
    const shader::ShaderArtifactView& artifact,
    uint32_t rootTypeIndex,
    uint32_t bufferIndex,
    uint32_t group,
    uint32_t bindingNumber,
    render::BindingHandle bindingHandle) {
    const std::span<const shader::WireTypeRecord> types = artifact.Types();
    if (rootTypeIndex >= types.size()) {
        RADRAY_ERR_LOG(
            "shader parameter layout references missing root type {} (type count {})",
            rootTypeIndex,
            types.size());
        return false;
    }

    std::function<bool(uint32_t, uint32_t, std::optional<ArrayContext>)> visitStruct;
    visitStruct = [&](uint32_t structIndex, uint32_t baseOffset, std::optional<ArrayContext> array) -> bool {
        if (structIndex >= types.size() ||
            types[structIndex].Kind != static_cast<uint32_t>(shader::ShaderTypeKind::Struct)) {
            RADRAY_ERR_LOG(
                "shader parameter layout expected struct type at index {}",
                structIndex);
            return false;
        }
        for (uint32_t childIndex = 0; childIndex < types.size(); ++childIndex) {
            const shader::WireTypeRecord& child = types[childIndex];
            if (child.ParentIndex != structIndex) {
                continue;
            }
            const std::optional<std::string_view> name = artifact.GetName(child.Name);
            if (!name.has_value() || name->empty() ||
                child.Offset > std::numeric_limits<uint32_t>::max() - baseOffset) {
                RADRAY_ERR_LOG(
                    "shader parameter layout found invalid child {} of struct type {}",
                    childIndex,
                    structIndex);
                return false;
            }
            const uint32_t absoluteOffset = baseOffset + child.Offset;
            const std::optional<ShaderParameterKind> leafKind = GetLeafKind(child.Kind);
            if (leafKind.has_value()) {
                const uint32_t count = array.has_value() ? array->Count : child.ElementCount;
                const uint32_t stride = array.has_value() ? array->Stride : child.Stride;
                if (!result.AddParameter(
                        string{name.value()},
                        ShaderParameterInfo{
                            .Kind = leafKind.value(),
                            .Binding = bindingHandle,
                            .Group = group,
                            .BindingNumber = bindingNumber,
                            .BufferIndex = bufferIndex,
                            .ByteOffset = absoluteOffset,
                            .Size = child.Size,
                            .Stride = stride,
                            .ElementCount = count})) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout rejected duplicate or invalid parameter '{}'",
                        name.value());
                    return false;
                }
                continue;
            }

            const shader::ShaderTypeKind kind =
                static_cast<shader::ShaderTypeKind>(child.Kind);
            if (kind == shader::ShaderTypeKind::Struct ||
                kind == shader::ShaderTypeKind::Member) {
                if (child.TypeIndex == shader::kShaderNoType ||
                    !visitStruct(child.TypeIndex, absoluteOffset, array)) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout could not expand composite parameter '{}'",
                        name.value());
                    return false;
                }
                continue;
            }
            if (kind == shader::ShaderTypeKind::Array) {
                if (array.has_value()) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout does not support nested array parameter '{}'",
                        name.value());
                    return false;
                }
                if (child.TypeIndex == shader::kShaderNoType) {
                    // Array of a non-struct element. The wire contract carries the
                    // stride and the count but never the element kind, so the slot is
                    // exposed as ShaderParameterKind::Raw instead of guessing a type.
                    if (!result.AddParameter(
                            string{name.value()},
                            ShaderParameterInfo{
                                .Kind = ShaderParameterKind::Raw,
                                .Binding = bindingHandle,
                                .Group = group,
                                .BindingNumber = bindingNumber,
                                .BufferIndex = bufferIndex,
                                .ByteOffset = absoluteOffset,
                                .Size = child.Stride,
                                .Stride = child.Stride,
                                .ElementCount = child.ElementCount})) {
                        RADRAY_ERR_LOG(
                            "shader parameter layout rejected duplicate or invalid parameter '{}'",
                            name.value());
                        return false;
                    }
                    continue;
                }
                if (!visitStruct(
                        child.TypeIndex,
                        absoluteOffset,
                        ArrayContext{child.ElementCount, child.Stride})) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout could not expand array parameter '{}'",
                        name.value());
                    return false;
                }
                continue;
            }
            RADRAY_ERR_LOG(
                "shader parameter layout found unsupported type kind {} for parameter '{}'",
                child.Kind,
                name.value());
            return false;
        }
        return true;
    };

    return visitStruct(rootTypeIndex, 0, std::nullopt);
}

template <typename T>
std::span<const byte> AsBytes(const T& value) noexcept {
    return std::span<const byte>{
        reinterpret_cast<const byte*>(&value),
        sizeof(T)};
}

}  // namespace

std::optional<ShaderParameterLayout> ShaderParameterLayout::Create(
    const render::BackendShaderArtifact& artifact) noexcept {
    if (artifact.Layout == nullptr) {
        return std::nullopt;
    }
    return Create(artifact.Generic(), artifact.Layout.get());
}

std::optional<ShaderParameterLayout> ShaderParameterLayout::Create(
    const shader::ShaderArtifactView& view,
    Nullable<render::PipelineLayout*> pipelineLayout) noexcept {
    vector<const shader::WireBindingRecord*> cbufferBindings;
    cbufferBindings.reserve(view.Bindings().size());
    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        if (binding.Type == static_cast<uint32_t>(shader::ShaderBindingKind::CBuffer)) {
            cbufferBindings.push_back(&binding);
        }
    }

    vector<uint8_t> referencedTypes(view.Types().size(), 0);
    for (const shader::WireTypeRecord& type : view.Types()) {
        if (type.TypeIndex != shader::kShaderNoType) {
            if (type.TypeIndex >= referencedTypes.size()) {
                RADRAY_ERR_LOG(
                    "shader parameter layout found out-of-range type reference {}",
                    type.TypeIndex);
                return std::nullopt;
            }
            referencedTypes[type.TypeIndex] = 1;
        }
    }
    vector<uint32_t> bufferRootTypes;
    for (uint32_t index = 0; index < view.Types().size(); ++index) {
        const shader::WireTypeRecord& type = view.Types()[index];
        if (type.ParentIndex == shader::kShaderNoType && referencedTypes[index] == 0 &&
            type.Kind == static_cast<uint32_t>(shader::ShaderTypeKind::Struct)) {
            bufferRootTypes.push_back(index);
        }
    }
    if (bufferRootTypes.size() != cbufferBindings.size()) {
        RADRAY_ERR_LOG(
            "shader parameter layout found {} cbuffer roots for {} cbuffer bindings",
            bufferRootTypes.size(),
            cbufferBindings.size());
        return std::nullopt;
    }

    ShaderParameterLayout result;
    result._buffers.reserve(cbufferBindings.size());
    // cbuffer bindings and cbuffer root types are matched by position, because a
    // WireBindingRecord carries no TypeIndex and the binding name is the variable name
    // while the root type name is the struct name, so the two cannot be matched by name.
    // This relies on the compiler emitting both sequences in declaration order.
    // multiple_cbuffers in test_material pins that ordering for both targets.
    for (uint32_t bufferIndex = 0; bufferIndex < cbufferBindings.size(); ++bufferIndex) {
        const shader::WireBindingRecord& binding = *cbufferBindings[bufferIndex];
        const std::optional<std::string_view> bindingName = view.GetName(binding.Name);
        if (!bindingName.has_value()) {
            RADRAY_ERR_LOG("shader parameter layout found a cbuffer with an invalid name");
            return std::nullopt;
        }
        const render::BindingHandle handle = pipelineLayout.HasValue()
                                                 ? pipelineLayout.Get()->FindBinding(bindingName.value())
                                                 : render::BindingHandle{};
        if (pipelineLayout.HasValue() && !handle.IsValid()) {
            RADRAY_ERR_LOG(
                "shader parameter layout could not resolve cbuffer binding '{}'",
                bindingName.value());
            return std::nullopt;
        }
        const shader::WireTypeRecord& root = view.Types()[bufferRootTypes[bufferIndex]];
        result._buffers.push_back(ShaderParameterBufferLayout{
            .Name = string{bindingName.value()},
            .Binding = handle,
            .Group = binding.Group,
            .BindingNumber = binding.Binding,
            .Size = root.Size});
        if (!BuildBufferParameters(
                result,
                view,
                bufferRootTypes[bufferIndex],
                bufferIndex,
                binding.Group,
                binding.Binding,
                handle)) {
            return std::nullopt;
        }
    }

    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        ShaderParameterKind kind{};
        const auto logicalKind = static_cast<shader::ShaderBindingKind>(binding.Type);
        if (logicalKind == shader::ShaderBindingKind::Texture) {
            kind = ShaderParameterKind::Texture;
        } else if (logicalKind == shader::ShaderBindingKind::Sampler &&
                   binding.Placement !=
                       static_cast<uint32_t>(shader::ShaderBindingPlacement::StaticSampler) &&
                   binding.SamplerIndex == shader::kShaderNoSampler) {
            // A sampler the policy already fixed is not a material parameter: on D3D12 it lives in
            // the serialized carrier and on Vulkan its state comes from a published record, so
            // neither one can be set by a caller.
            kind = ShaderParameterKind::Sampler;
        } else {
            continue;
        }
        const std::optional<std::string_view> name = view.GetName(binding.Name);
        if (!name.has_value()) {
            RADRAY_ERR_LOG("shader parameter layout found a resource with an invalid name");
            return std::nullopt;
        }
        const render::BindingHandle handle = pipelineLayout.HasValue()
                                                 ? pipelineLayout.Get()->FindBinding(name.value())
                                                 : render::BindingHandle{};
        if ((pipelineLayout.HasValue() && !handle.IsValid()) || !result.AddParameter(
                                                                    string{name.value()},
                                                                    ShaderParameterInfo{
                                                                        .Kind = kind,
                                                                        .Binding = handle,
                                                                        .Group = binding.Group,
                                                                        .BindingNumber = binding.Binding,
                                                                        .BufferIndex = kNoIndex,
                                                                        .ByteOffset = 0,
                                                                        .Size = 0,
                                                                        .Stride = 0,
                                                                        .ElementCount = binding.Count})) {
            RADRAY_ERR_LOG(
                "shader parameter layout rejected resource parameter '{}'",
                name.value());
            return std::nullopt;
        }
    }
    return result;
}

const ShaderParameterInfo* ShaderParameterLayout::Find(
    std::string_view name) const noexcept {
    const auto found = _parameterIndices.find(name);
    if (found == _parameterIndices.end()) {
        return nullptr;
    }
    return &_parameters[found->second].Info;
}

bool ShaderParameterLayout::AddParameter(string name, ShaderParameterInfo info) {
    if (_parameterIndices.contains(name)) {
        return false;
    }
    const size_t index = _parameters.size();
    _parameters.push_back(ShaderParameterRecord{
        .Name = std::move(name),
        .Info = info});
    _parameterIndices.emplace(_parameters.back().Name, index);
    return true;
}

ShaderParameterStorage::ShaderParameterStorage(
    const ShaderParameterLayout* layout)
    : _layout(layout) {
    if (_layout == nullptr) {
        return;
    }
    _bufferData.reserve(_layout->Buffers().size());
    for (const ShaderParameterBufferLayout& buffer : _layout->Buffers()) {
        _bufferData.emplace_back(buffer.Size, byte{0});
    }
}

void ShaderParameterStorage::Reset() noexcept {
    for (vector<byte>& buffer : _bufferData) {
        std::fill(buffer.begin(), buffer.end(), byte{0});
    }
}

std::span<const byte> ShaderParameterStorage::GetBufferData(
    uint32_t bufferIndex) const noexcept {
    if (bufferIndex >= _bufferData.size()) {
        return {};
    }
    return _bufferData[bufferIndex];
}

bool ShaderParameterStorage::SetFloat(
    std::string_view name, float value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool ShaderParameterStorage::SetFloat2(
    std::string_view name, const Eigen::Vector2f& value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool ShaderParameterStorage::SetFloat3(
    std::string_view name, const Eigen::Vector3f& value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool ShaderParameterStorage::SetFloat4(
    std::string_view name, const Eigen::Vector4f& value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Vector, AsBytes(value), element);
}

bool ShaderParameterStorage::SetInt(
    std::string_view name, int32_t value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool ShaderParameterStorage::SetUInt(
    std::string_view name, uint32_t value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Scalar, AsBytes(value), element);
}

bool ShaderParameterStorage::SetMatrix4x4(
    std::string_view name, const Eigen::Matrix4f& value, uint32_t element) noexcept {
    return SetBytes(name, ShaderParameterKind::Matrix, AsBytes(value), element);
}

bool ShaderParameterStorage::SetRaw(
    std::string_view name,
    std::span<const byte> value,
    uint32_t element) noexcept {
    if (_layout == nullptr) {
        return false;
    }
    const ShaderParameterInfo* parameter = _layout->Find(name);
    if (parameter == nullptr || parameter->Kind != ShaderParameterKind::Raw ||
        value.empty() || value.size() > parameter->Size ||
        element >= parameter->ElementCount ||
        parameter->BufferIndex >= _bufferData.size()) {
        return false;
    }
    vector<byte>& target = _bufferData[parameter->BufferIndex];
    const uint64_t offset = static_cast<uint64_t>(parameter->ByteOffset) +
                            static_cast<uint64_t>(element) * parameter->Stride;
    if (offset > target.size() || value.size() > target.size() - offset) {
        return false;
    }
    std::memcpy(target.data() + offset, value.data(), value.size());
    return true;
}

bool ShaderParameterStorage::SetBytes(
    std::string_view name,
    ShaderParameterKind expectedKind,
    std::span<const byte> value,
    uint32_t element) noexcept {
    if (_layout == nullptr) {
        return false;
    }
    const ShaderParameterInfo* parameter = _layout->Find(name);
    if (parameter == nullptr || parameter->Kind != expectedKind ||
        parameter->Size != value.size() || element >= parameter->ElementCount ||
        parameter->BufferIndex >= _bufferData.size()) {
        return false;
    }
    vector<byte>& target = _bufferData[parameter->BufferIndex];
    const uint64_t offset = static_cast<uint64_t>(parameter->ByteOffset) +
                            static_cast<uint64_t>(element) * parameter->Stride;
    if (offset > target.size() || value.size() > target.size() - offset) {
        return false;
    }
    std::memcpy(target.data() + offset, value.data(), value.size());
    return true;
}

}  // namespace radray

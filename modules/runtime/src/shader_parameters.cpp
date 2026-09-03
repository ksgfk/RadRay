#include <radray/runtime/shader_parameters.h>

#include <algorithm>
#include <cstring>

#include <radray/logger.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray {
namespace {

constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

constexpr size_t kAmbiguousParameterIndex = std::numeric_limits<size_t>::max();
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
    std::string_view declarationName,
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

    string path{declarationName};
    const auto visitStruct = [&](auto&& self,
                                 uint32_t structIndex,
                                 uint32_t baseOffset,
                                 std::optional<ArrayContext> array) -> bool {
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
            const size_t parentPathSize = path.size();
            path.push_back('.');
            path.append(name.value());
            const std::optional<ShaderParameterKind> leafKind = GetLeafKind(child.Kind);
            if (leafKind.has_value()) {
                const uint32_t count = array.has_value() ? array->Count : child.ElementCount;
                const uint32_t stride = array.has_value() ? array->Stride : child.Stride;
                if (!result.AddParameter(
                        path,
                        name.value(),
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
                        path);
                    return false;
                }
                path.resize(parentPathSize);
                continue;
            }

            const shader::ShaderTypeKind kind =
                static_cast<shader::ShaderTypeKind>(child.Kind);
            if (kind == shader::ShaderTypeKind::Struct ||
                kind == shader::ShaderTypeKind::Member) {
                if (child.TypeIndex == shader::kShaderNoType ||
                    !self(self, child.TypeIndex, absoluteOffset, array)) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout could not expand composite parameter '{}'",
                        path);
                    return false;
                }
                path.resize(parentPathSize);
                continue;
            }
            if (kind == shader::ShaderTypeKind::Array) {
                if (array.has_value()) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout does not support nested array parameter '{}'",
                        path);
                    return false;
                }
                if (child.TypeIndex == shader::kShaderNoType) {
                    // Array of a non-struct element. The wire contract carries the
                    // stride and the count but never the element kind, so the slot is
                    // exposed as ShaderParameterKind::Raw instead of guessing a type.
                    if (!result.AddParameter(
                            path,
                            name.value(),
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
                            path);
                        return false;
                    }
                    path.resize(parentPathSize);
                    continue;
                }
                if (!self(
                        self,
                        child.TypeIndex,
                        absoluteOffset,
                        ArrayContext{child.ElementCount, child.Stride})) {
                    RADRAY_ERR_LOG(
                        "shader parameter layout could not expand array parameter '{}'",
                        path);
                    return false;
                }
                path.resize(parentPathSize);
                continue;
            }
            RADRAY_ERR_LOG(
                "shader parameter layout found unsupported type kind {} for parameter '{}'",
                child.Kind,
                path);
            return false;
        }
        return true;
    };

    return visitStruct(visitStruct, rootTypeIndex, 0, std::nullopt);
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
    size_t cbufferCount = 0;
    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        cbufferCount +=
            binding.Type == static_cast<uint32_t>(shader::ShaderBindingKind::CBuffer) ? 1 : 0;
    }

    ShaderParameterLayout result;
    result._buffers.reserve(cbufferCount);
    for (const shader::WireBindingRecord& binding : view.Bindings()) {
        const shader::ShaderBindingKind logicalKind =
            static_cast<shader::ShaderBindingKind>(binding.Type);
        ShaderParameterKind parameterKind{};
        if (logicalKind == shader::ShaderBindingKind::CBuffer) {
            if (binding.TypeIndex == shader::kShaderNoType ||
                binding.TypeIndex >= view.Types().size()) {
                RADRAY_ERR_LOG(
                    "shader parameter layout found cbuffer owner {} outside {} type records",
                    binding.TypeIndex,
                    view.Types().size());
                return std::nullopt;
            }
            const shader::WireTypeRecord& root = view.Types()[binding.TypeIndex];
            if (root.ParentIndex != shader::kShaderNoType ||
                root.Kind != static_cast<uint32_t>(shader::ShaderTypeKind::Struct)) {
                RADRAY_ERR_LOG(
                    "shader parameter layout expected cbuffer owner {} to be a top-level struct",
                    binding.TypeIndex);
                return std::nullopt;
            }
            const std::optional<std::string_view> bindingName = view.GetName(binding.Name);
            if (!bindingName.has_value()) {
                RADRAY_ERR_LOG("shader parameter layout found a cbuffer with an invalid name");
                return std::nullopt;
            }
            const render::BindingHandle handle = pipelineLayout.HasValue()
                                                     ? pipelineLayout.Get()->FindBinding(
                                                           bindingName.value())
                                                     : render::BindingHandle{};
            if (pipelineLayout.HasValue() && !handle.IsValid()) {
                RADRAY_ERR_LOG(
                    "shader parameter layout could not resolve cbuffer binding '{}'",
                    bindingName.value());
                return std::nullopt;
            }
            const uint32_t bufferIndex = static_cast<uint32_t>(result._buffers.size());
            result._buffers.push_back(ShaderParameterBufferLayout{
                .Name = string{bindingName.value()},
                .Binding = handle,
                .Group = binding.Group,
                .BindingNumber = binding.Binding,
                .Size = root.Size});
            if (!BuildBufferParameters(
                    result,
                    view,
                    binding.TypeIndex,
                    bindingName.value(),
                    bufferIndex,
                    binding.Group,
                    binding.Binding,
                    handle)) {
                return std::nullopt;
            }
            continue;
        }
        if (logicalKind == shader::ShaderBindingKind::Texture) {
            parameterKind = ShaderParameterKind::Texture;
        } else if (
            logicalKind == shader::ShaderBindingKind::Sampler &&
            binding.Placement !=
                static_cast<uint32_t>(shader::ShaderBindingPlacement::StaticSampler) &&
            binding.SamplerIndex == shader::kShaderNoSampler) {
            // A sampler the policy already fixed is not a material parameter: on D3D12 it lives in
            // the serialized carrier and on Vulkan its state comes from a published record, so
            // neither one can be set by a caller.
            parameterKind = ShaderParameterKind::Sampler;
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
        if ((pipelineLayout.HasValue() && !handle.IsValid()) ||
            !result.AddParameter(
                string{name.value()},
                name.value(),
                ShaderParameterInfo{
                    .Kind = parameterKind,
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
    const auto exact = _parameterIndices.find(name);
    if (exact != _parameterIndices.end()) {
        return &_parameters[exact->second].Info;
    }
    const auto shorthand = _shortParameterIndices.find(name);
    if (shorthand == _shortParameterIndices.end() ||
        shorthand->second == kAmbiguousParameterIndex) {
        return nullptr;
    }
    return &_parameters[shorthand->second].Info;
}

bool ShaderParameterLayout::AddParameter(
    string canonicalPath,
    std::string_view leafName,
    ShaderParameterInfo info) {
    if (canonicalPath.empty() || leafName.empty() ||
        _parameterIndices.contains(canonicalPath)) {
        return false;
    }
    const size_t index = _parameters.size();
    _parameters.push_back(ShaderParameterRecord{
        .Name = std::move(canonicalPath),
        .Info = info});
    _parameterIndices.emplace(_parameters.back().Name, index);
    const auto [shortEntry, inserted] =
        _shortParameterIndices.emplace(string{leafName}, index);
    if (!inserted) {
        shortEntry->second = kAmbiguousParameterIndex;
    }
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

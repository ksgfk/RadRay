#include <radray/runtime/material_parameter_binding_map.h>

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <variant>

#include <radray/runtime/material.h>
#include <radray/runtime/shader_asset.h>
#include <radray/logger.h>

namespace radray {
namespace {

struct ExpectedConstantLayout {
    shader::ShaderTypeKind Kind;
    shader::ShaderScalarKind ScalarKind;
    uint32_t RowCount;
    uint32_t ColumnCount;
    uint32_t Size;
    bool IsMatrix4x4{false};
};

std::optional<ExpectedConstantLayout> DescribeConstantValue(const MaterialParameterValue& value) noexcept {
    return std::visit(
        []<class T>(const T&) noexcept -> std::optional<ExpectedConstantLayout> {
            if constexpr (std::is_same_v<T, float>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Scalar, shader::ShaderScalarKind::Float, 1, 1, 4};
            else if constexpr (std::is_same_v<T, int32_t>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Scalar, shader::ShaderScalarKind::SignedInteger, 1, 1, 4};
            else if constexpr (std::is_same_v<T, uint32_t>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Scalar, shader::ShaderScalarKind::UnsignedInteger, 1, 1, 4};
            else if constexpr (std::is_same_v<T, array<float, 2>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Vector, shader::ShaderScalarKind::Float, 1, 2, 8};
            else if constexpr (std::is_same_v<T, array<float, 3>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Vector, shader::ShaderScalarKind::Float, 1, 3, 12};
            else if constexpr (std::is_same_v<T, array<float, 4>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Vector, shader::ShaderScalarKind::Float, 1, 4, 16};
            else if constexpr (std::is_same_v<T, array<int32_t, 4>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Vector, shader::ShaderScalarKind::SignedInteger, 1, 4, 16};
            else if constexpr (std::is_same_v<T, array<uint32_t, 4>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Vector, shader::ShaderScalarKind::UnsignedInteger, 1, 4, 16};
            else if constexpr (std::is_same_v<T, array<float, 16>>)
                return ExpectedConstantLayout{shader::ShaderTypeKind::Matrix, shader::ShaderScalarKind::Float, 4, 4, 64, true};
            else
                return std::nullopt;
        },
        value);
}

}  // namespace

std::optional<MaterialParameterBindingMap> MaterialParameterBindingMap::Create(std::span<const MaterialParameter> parameters, const ShaderAsset& layout,
                                                                               std::span<const string> subset, std::string_view constantBlockName) {
    MaterialParameterBindingMap result;
    for (const auto& group : layout.GetLayoutDesc().Groups)
        if (std::any_of(group.Bindings.begin(), group.Bindings.end(), [&](const auto& entry) {
                return layout.GetBackend() == gpu::GpuBackend::D3D12 ? entry.Binding.D3D12.has_value() : entry.Binding.Vulkan.has_value();
            })) result._activeGroups.push_back(group.GroupSlot);
    unordered_set<string> names;
    for (const auto& name : subset) {
        const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) { return parameter.Name == name; });
        if (found == parameters.end() || !names.insert(name).second) {
            RADRAY_ERR_LOG("Unknown or duplicate material subset parameter: {}", name);
            return std::nullopt;
        }
        const size_t index = static_cast<size_t>(found - parameters.begin());
        const auto& parameter = *found;
        if (const auto expected = DescribeConstantValue(parameter.DefaultValue)) {
            if (constantBlockName.empty()) {
                RADRAY_ERR_LOG("Material constants need a block name");
                return std::nullopt;
            }
            if (!result._constantBlockSize) {
                const auto buffer = layout.FindBinding(constantBlockName);
                const auto push = layout.FindPushConstant(constantBlockName);
                if (buffer && buffer->Kind == gpu::BindingKind::CBuffer && buffer->ActiveOnDevice)
                    result._constantBlockSize = buffer->BufferByteSize;
                else if (push && push->ActiveOnDevice)
                    result._constantBlockSize = push->Size;
                else {
                    RADRAY_ERR_LOG("Material constant block is unavailable: {}", constantBlockName);
                    return std::nullopt;
                }
            }
            const auto member = layout.FindBufferMember(constantBlockName, parameter.Name);
            if (!member || member->Kind != expected->Kind || member->ScalarKind != expected->ScalarKind ||
                member->ElementCount != 1 || member->RowCount != expected->RowCount ||
                member->ColumnCount != expected->ColumnCount || member->Size != expected->Size ||
                member->HasInternalPadding() || (expected->IsMatrix4x4 && member->MatrixStride != 16)) {
                RADRAY_ERR_LOG("Material constant type mismatch: {}", parameter.Name);
                return std::nullopt;
            }
            const bool transpose = expected->IsMatrix4x4 && member->RowMajor;
            result._constants.push_back({index, member->AbsoluteOffset, expected->Size, parameter.DefaultValue.index(), transpose});
        } else {
            const auto binding = layout.FindBinding(parameter.Name);
            const bool texture = std::holds_alternative<Nullable<gpu::TextureView*>>(parameter.DefaultValue);
            if (!binding || binding->Count != 1 || binding->ImmutableSampler ||
                (texture ? (binding->Kind != gpu::BindingKind::Texture && binding->Kind != gpu::BindingKind::RWTexture)
                         : binding->Kind != gpu::BindingKind::Sampler)) {
                RADRAY_ERR_LOG("Material resource type mismatch: {}", parameter.Name);
                return std::nullopt;
            }
            result._resources.push_back({index, binding->Reference, binding->Kind, binding->ActiveOnDevice});
        }
    }
    return result;
}

std::span<const MaterialParameterResourceTarget> MaterialParameterBindingMap::GetResourceTargets() const noexcept { return _resources; }
std::span<const MaterialParameterConstantTarget> MaterialParameterBindingMap::GetConstantTargets() const noexcept { return _constants; }

bool MaterialParameterBindingMap::WriteConstants(const MaterialRenderProxy& proxy, std::span<byte> destination) const noexcept {
#if RADRAY_IS_DEBUG
    RADRAY_ASSERT(proxy.GetMaterial() == _material);
#endif
    if (destination.size() < _constantBlockSize) return false;
    for (const auto& target : _constants) {
        const auto parameter = proxy.GetParameter(target.ParameterIndex);
        if (!parameter || target.AbsoluteOffset > destination.size() || target.Size > destination.size() - target.AbsoluteOffset ||
            parameter->index() != target.ExpectedVariantIndex) return false;
    }
    for (const auto& target : _constants) {
        const auto& parameter = *proxy.GetParameter(target.ParameterIndex).Get();
        byte* dst = destination.data() + target.AbsoluteOffset;
        std::visit(
            [&]<class T>(const T& val) noexcept {
                if constexpr (std::is_same_v<T, array<float, 16>>) {
                    if (!target.TransposeMatrix4x4) {
                        std::memcpy(dst, val.data(), 64);
                    } else {
                        for (uint32_t row = 0; row < 4; ++row) {
                            for (uint32_t col = 0; col < 4; ++col) {
                                std::memcpy(dst + row * 16 + col * 4, &val[col * 4 + row], sizeof(float));
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>) {
                    std::memcpy(dst, &val, sizeof(T));
                } else if constexpr (std::is_same_v<T, array<float, 2>> || std::is_same_v<T, array<float, 3>> || std::is_same_v<T, array<float, 4>> ||
                                     std::is_same_v<T, array<int32_t, 4>> || std::is_same_v<T, array<uint32_t, 4>>) {
                    std::memcpy(dst, val.data(), sizeof(T));
                }
            },
            parameter);
    }
    return true;
}

bool MaterialParameterBindingMap::BindResources(const MaterialRenderProxy& proxy, uint32_t groupSlot, gpu::ShaderParameterSet& parameterSet) const noexcept {
#if RADRAY_IS_DEBUG
    RADRAY_ASSERT(proxy.GetMaterial() == _material);
#endif
    if (std::find(_activeGroups.begin(), _activeGroups.end(), groupSlot) == _activeGroups.end()) return false;
    for (const auto& target : _resources) {
        if (!target.ActiveOnDevice || target.Reference.GroupSlot != groupSlot) continue;
        const auto parameter = proxy.GetParameter(target.ParameterIndex);
        if (!parameter) return false;
        if (target.Kind == gpu::BindingKind::Sampler) {
            const auto* sampler = std::get_if<Nullable<gpu::Sampler*>>(parameter.Get());
            if (!sampler || !*sampler) return false;
        } else {
            const auto* texture = std::get_if<Nullable<gpu::TextureView*>>(parameter.Get());
            if (!texture || !*texture) return false;
        }
    }
    for (const auto& target : _resources) {
        if (!target.ActiveOnDevice || target.Reference.GroupSlot != groupSlot) continue;
        const auto& parameter = *proxy.GetParameter(target.ParameterIndex).Get();
        if (target.Kind == gpu::BindingKind::Sampler) {
            if (!parameterSet.Set(target.Reference.BindingId, 0, std::get<Nullable<gpu::Sampler*>>(parameter).Get())) return false;
        } else if (!parameterSet.Set(target.Reference.BindingId, 0, std::get<Nullable<gpu::TextureView*>>(parameter).Get()))
            return false;
    }
    return true;
}

}  // namespace radray

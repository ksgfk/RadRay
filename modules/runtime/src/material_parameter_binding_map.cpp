#include <radray/runtime/material_parameter_binding_map.h>

#include <cstring>
#include <algorithm>
#include <radray/runtime/material.h>
#include <radray/logger.h>

namespace radray {

std::optional<MaterialParameterBindingMap> MaterialParameterBindingMap::Create(std::span<const MaterialParameter> parameters, const ShaderAsset& layout,
                                                                               std::span<const string> subset, std::string_view constantBlockName) {
    MaterialParameterBindingMap result;
    for (const auto& group : layout.GetLayoutDesc().Groups)
        if (std::any_of(group.Bindings.begin(), group.Bindings.end(), [&](const auto& entry) {
                return layout.GetBackend() == gpu::RenderBackend::D3D12 ? entry.Binding.D3D12.has_value() : entry.Binding.Vulkan.has_value();
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
        if (std::holds_alternative<float>(parameter.DefaultValue) || std::holds_alternative<array<float, 4>>(parameter.DefaultValue)) {
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
            const bool scalar = std::holds_alternative<float>(parameter.DefaultValue);
            const uint32_t size = scalar ? 4 : 16;
            if (!member || member->Kind != (scalar ? shader::ShaderTypeKind::Scalar : shader::ShaderTypeKind::Vector) ||
                member->ScalarKind != shader::ShaderScalarKind::Float || member->ElementCount != 1 || member->RowCount != 1 ||
                member->ColumnCount != (scalar ? 1u : 4u) || member->Size != size || member->HasInternalPadding()) {
                RADRAY_ERR_LOG("Material constant type mismatch: {}", parameter.Name);
                return std::nullopt;
            }
            result._constants.push_back({index, member->AbsoluteOffset, size});
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
            (target.Size == 4 ? !std::holds_alternative<float>(*parameter.Get()) : !std::holds_alternative<array<float, 4>>(*parameter.Get()))) return false;
    }
    for (const auto& target : _constants) {
        const auto& parameter = *proxy.GetParameter(target.ParameterIndex).Get();
        if (target.Size == 4)
            std::memcpy(destination.data() + target.AbsoluteOffset, &std::get<float>(parameter), 4);
        else
            std::memcpy(destination.data() + target.AbsoluteOffset, std::get<array<float, 4>>(parameter).data(), 16);
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

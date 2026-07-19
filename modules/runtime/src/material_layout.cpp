#include <radray/runtime/material_layout.h>

#include <algorithm>

namespace radray {

namespace {

using namespace shader;

bool AddField(MaterialBindingDesc& binding, MaterialFieldDesc field) {
    const auto sameName = std::ranges::find_if(binding.Fields, [&](const MaterialFieldDesc& value) {
        return value.Name == field.Name;
    });
    const auto sameOffset = std::ranges::find_if(binding.Fields, [&](const MaterialFieldDesc& value) {
        return value.Offset == field.Offset;
    });
    if (sameName == binding.Fields.end() && sameOffset == binding.Fields.end()) {
        binding.Fields.emplace_back(std::move(field));
        return true;
    }
    return sameName != binding.Fields.end() && sameOffset != binding.Fields.end() &&
           *sameName == field && *sameOffset == field;
}

bool MergeBinding(MaterialLayout& layout, MaterialBindingDesc incoming) {
    const auto sameName = std::ranges::find_if(layout.Bindings, [&](const MaterialBindingDesc& value) {
        return value.Name == incoming.Name;
    });
    const auto sameBinding = std::ranges::find_if(layout.Bindings, [&](const MaterialBindingDesc& value) {
        return value.Binding == incoming.Binding;
    });
    if (sameName == layout.Bindings.end() && sameBinding == layout.Bindings.end()) {
        layout.Bindings.emplace_back(std::move(incoming));
        return true;
    }
    if (sameName == layout.Bindings.end() || sameBinding == layout.Bindings.end() || sameName != sameBinding) {
        return false;
    }
    MaterialBindingDesc& current = *sameName;
    if (current.Kind != incoming.Kind || current.Count != incoming.Count || current.ByteSize != incoming.ByteSize) {
        return false;
    }
    for (MaterialFieldDesc& field : incoming.Fields) {
        if (!AddField(current, std::move(field))) {
            return false;
        }
    }
    return true;
}

bool AppendHlslMembers(
    const HlslShaderDesc& reflection,
    HlslShaderTypeId typeId,
    uint32_t baseOffset,
    std::string_view prefix,
    vector<MaterialFieldDesc>& fields,
    size_t depth = 0) {
    if (static_cast<size_t>(typeId) >= reflection.Types.size() || depth >= reflection.Types.size()) {
        return false;
    }
    const HlslShaderTypeDesc& type = reflection.Types[static_cast<size_t>(typeId)];
    for (const HlslShaderTypeMember& member : type.Members) {
        if (static_cast<size_t>(member.Type) >= reflection.Types.size()) {
            return false;
        }
        const HlslShaderTypeDesc& memberType = reflection.Types[static_cast<size_t>(member.Type)];
        string name{prefix};
        if (!name.empty()) {
            name.push_back('.');
        }
        name.append(member.Name);
        if (!memberType.Members.empty() && memberType.Elements == 0) {
            if (!AppendHlslMembers(
                    reflection,
                    member.Type,
                    baseOffset + memberType.Offset,
                    name,
                    fields,
                    depth + 1)) {
                return false;
            }
            continue;
        }
        const size_t size = memberType.GetSizeInBytes();
        if (size == 0 || size > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        fields.emplace_back(MaterialFieldDesc{
            .Name = std::move(name),
            .Offset = baseOffset + memberType.Offset,
            .Size = static_cast<uint32_t>(size)});
    }
    return true;
}

bool AppendHlslLayout(
    const HlslShaderDesc& reflection,
    uint32_t materialGroup,
    MaterialLayout& layout) {
    for (const HlslInputBindDesc& resource : reflection.BoundResources) {
        const uint32_t group = resource.VkSet.value_or(resource.Space);
        if (group != materialGroup) {
            continue;
        }
        const uint32_t binding = resource.VkBinding.value_or(resource.BindPoint);
        if (resource.BindCount != 1) {
            return false;
        }

        MaterialBindingDesc result{
            .Name = resource.Name,
            .Kind = MaterialBindingKind::ConstantBuffer,
            .Binding = binding,
            .Count = resource.BindCount,
            .ByteSize = 0,
            .Fields = {}};
        if (resource.Type == HlslShaderInputType::CBUFFER) {
            result.Kind = MaterialBindingKind::ConstantBuffer;
            const auto buffer = reflection.FindCBufferByName(resource.Name);
            if (!buffer.HasValue() || buffer.Get()->Size == 0) {
                return false;
            }
            result.ByteSize = buffer.Get()->Size;
            for (const size_t variableIndex : buffer.Get()->Variables) {
                if (variableIndex >= reflection.Variables.size()) {
                    return false;
                }
                const HlslShaderVariableDesc& variable = reflection.Variables[variableIndex];
                if (static_cast<size_t>(variable.Type) < reflection.Types.size() &&
                    !reflection.Types[static_cast<size_t>(variable.Type)].Members.empty()) {
                    if (!AppendHlslMembers(
                            reflection,
                            variable.Type,
                            variable.StartOffset,
                            {},
                            result.Fields)) {
                        return false;
                    }
                } else {
                    result.Fields.emplace_back(MaterialFieldDesc{
                        .Name = variable.Name,
                        .Offset = variable.StartOffset,
                        .Size = variable.Size});
                }
            }
        } else if (resource.Type == HlslShaderInputType::TEXTURE && !IsBufferDimension(resource.Dimension)) {
            result.Kind = MaterialBindingKind::Texture;
        } else if (resource.Type == HlslShaderInputType::SAMPLER) {
            result.Kind = MaterialBindingKind::Sampler;
        } else {
            return false;
        }
        if (!MergeBinding(layout, std::move(result))) {
            return false;
        }
    }
    return true;
}

bool AppendSpirvMembers(
    const SpirvShaderDesc& reflection,
    uint32_t typeIndex,
    uint32_t baseOffset,
    std::string_view prefix,
    vector<MaterialFieldDesc>& fields,
    size_t depth = 0) {
    if (typeIndex >= reflection.Types.size() || depth >= reflection.Types.size()) {
        return false;
    }
    const SpirvTypeInfo& type = reflection.Types[typeIndex];
    for (const SpirvTypeMember& member : type.Members) {
        string name{prefix};
        if (!name.empty()) {
            name.push_back('.');
        }
        name.append(member.Name);
        if (member.TypeIndex < reflection.Types.size()) {
            const SpirvTypeInfo& memberType = reflection.Types[member.TypeIndex];
            if (memberType.BaseType == SpirvBaseType::Struct && memberType.ArraySize == 0 &&
                !memberType.Members.empty()) {
                if (!AppendSpirvMembers(
                        reflection,
                        member.TypeIndex,
                        baseOffset + member.Offset,
                        name,
                        fields,
                        depth + 1)) {
                    return false;
                }
                continue;
            }
        }
        if (member.Size == 0) {
            return false;
        }
        fields.emplace_back(MaterialFieldDesc{
            .Name = std::move(name),
            .Offset = baseOffset + member.Offset,
            .Size = member.Size});
    }
    return true;
}

bool AppendSpirvLayout(
    const SpirvShaderDesc& reflection,
    uint32_t materialGroup,
    MaterialLayout& layout) {
    for (const SpirvResourceBinding& resource : reflection.ResourceBindings) {
        if (resource.Set != materialGroup) {
            continue;
        }
        if (resource.IsUnboundedArray) {
            return false;
        }
        MaterialBindingDesc result{
            .Name = resource.Name,
            .Kind = MaterialBindingKind::ConstantBuffer,
            .Binding = resource.Binding,
            .Count = resource.ArraySize == 0 ? 1u : resource.ArraySize,
            .ByteSize = 0,
            .Fields = {}};
        if (result.Count != 1) {
            return false;
        }
        switch (resource.Kind) {
            case SpirvResourceKind::UniformBuffer:
                result.Kind = MaterialBindingKind::ConstantBuffer;
                result.ByteSize = resource.UniformBufferSize;
                if (!AppendSpirvMembers(reflection, resource.TypeIndex, 0, {}, result.Fields)) {
                    return false;
                }
                break;
            case SpirvResourceKind::SampledImage:
            case SpirvResourceKind::SeparateImage:
                result.Kind = MaterialBindingKind::Texture;
                break;
            case SpirvResourceKind::SeparateSampler:
                result.Kind = MaterialBindingKind::Sampler;
                break;
            default:
                return false;
        }
        if (!MergeBinding(layout, std::move(result))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool MaterialLayout::IsValid() const noexcept {
    for (size_t i = 0; i < Bindings.size(); ++i) {
        const MaterialBindingDesc& binding = Bindings[i];
        if (binding.Name.empty() || binding.Count != 1) {
            return false;
        }
        if (binding.Kind == MaterialBindingKind::ConstantBuffer) {
            if (binding.ByteSize == 0) {
                return false;
            }
        } else if (binding.ByteSize != 0 || !binding.Fields.empty()) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (Bindings[j].Name == binding.Name || Bindings[j].Binding == binding.Binding) {
                return false;
            }
        }
        for (size_t j = 0; j < binding.Fields.size(); ++j) {
            const MaterialFieldDesc& field = binding.Fields[j];
            if (field.Name.empty() || field.Size == 0 || field.Offset >= binding.ByteSize ||
                field.Size > binding.ByteSize - field.Offset) {
                return false;
            }
            for (size_t k = 0; k < j; ++k) {
                const MaterialFieldDesc& other = binding.Fields[k];
                const uint32_t fieldEnd = field.Offset + field.Size;
                const uint32_t otherEnd = other.Offset + other.Size;
                if (other.Name == field.Name ||
                    (field.Offset < otherEnd && other.Offset < fieldEnd)) {
                    return false;
                }
            }
        }
    }
    return true;
}

Nullable<const MaterialBindingDesc*> MaterialLayout::FindBinding(std::string_view name) const noexcept {
    const auto it = std::ranges::find_if(Bindings, [name](const MaterialBindingDesc& binding) {
        return binding.Name == name;
    });
    return it == Bindings.end() ? nullptr : &*it;
}

Nullable<const MaterialBindingDesc*> MaterialLayout::FindBinding(uint32_t binding) const noexcept {
    const auto it = std::ranges::find_if(Bindings, [binding](const MaterialBindingDesc& value) {
        return value.Binding == binding;
    });
    return it == Bindings.end() ? nullptr : &*it;
}

std::optional<MaterialLayout> BuildMaterialLayout(
    std::span<const shader::CompiledShaderStage> stages,
    uint32_t bindingGroup) noexcept {
    try {
        if (stages.empty()) {
            return std::nullopt;
        }
        MaterialLayout result;
        result.Group = bindingGroup;
        for (const shader::CompiledShaderStage& stage : stages) {
            if (!stage.Reflection.has_value()) {
                return std::nullopt;
            }
            const bool valid = std::visit(
                [&](const auto& reflection) {
                    using T = std::decay_t<decltype(reflection)>;
                    if constexpr (std::is_same_v<T, shader::HlslShaderDesc>) {
                        return AppendHlslLayout(reflection, bindingGroup, result);
                    } else {
                        return AppendSpirvLayout(reflection, bindingGroup, result);
                    }
                },
                *stage.Reflection);
            if (!valid) {
                return std::nullopt;
            }
        }
        std::ranges::sort(result.Bindings, {}, &MaterialBindingDesc::Binding);
        for (MaterialBindingDesc& binding : result.Bindings) {
            std::ranges::sort(binding.Fields, {}, &MaterialFieldDesc::Offset);
        }
        return result.IsValid() ? std::optional<MaterialLayout>{std::move(result)} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace radray

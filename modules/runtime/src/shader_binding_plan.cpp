#include <radray/runtime/shader_binding_plan.h>

#include <algorithm>
#include <limits>
#include <tuple>
#include <variant>

#include <fmt/format.h>

#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

namespace radray {
namespace {

struct ReflectedConstantBlock {
    string Name;
    render::ShaderBindingLocation Location{};
    uint32_t Size{0};
    vector<ShaderConstantFieldPlan> Fields;
};

ShaderBindingFrequency MapFrequency(ShaderParameterScope scope) noexcept {
    switch (scope) {
        case ShaderParameterScope::Material: return ShaderBindingFrequency::Material;
        case ShaderParameterScope::Pass: return ShaderBindingFrequency::Pass;
        case ShaderParameterScope::View: return ShaderBindingFrequency::View;
        case ShaderParameterScope::Object: return ShaderBindingFrequency::Object;
    }
    return ShaderBindingFrequency::Object;
}

ShaderBindingFrequency MaxFrequency(
    ShaderBindingFrequency lhs,
    ShaderBindingFrequency rhs) noexcept {
    return static_cast<uint8_t>(lhs) >= static_cast<uint8_t>(rhs) ? lhs : rhs;
}

const ShaderParameterSourceDesc* FindSource(
    const ShaderPassDesc& pass,
    std::string_view block,
    std::string_view name) noexcept {
    if (!block.empty()) {
        const string qualified = fmt::format("{}.{}", block, name);
        auto qualifiedIt = std::ranges::find(pass.ParameterSources, qualified, &ShaderParameterSourceDesc::Name);
        if (qualifiedIt != pass.ParameterSources.end()) {
            return &*qualifiedIt;
        }
    }
    auto it = std::ranges::find(pass.ParameterSources, name, &ShaderParameterSourceDesc::Name);
    return it != pass.ParameterSources.end() ? &*it : nullptr;
}

ShaderBindingSource MakeSource(
    const ShaderParameterSourceDesc* source,
    std::string_view fallbackName) {
    if (source == nullptr) {
        return ShaderBindingSource{
            .Scope = ShaderParameterScope::Material,
            .ProviderName = string{fallbackName},
            .Explicit = false};
    }
    return ShaderBindingSource{
        .Scope = source->Scope,
        .ProviderName = source->ProviderName.empty() ? source->Name : source->ProviderName,
        .Explicit = true};
}

void AppendHlslBlocks(
    const render::HlslShaderDesc& hlsl,
    vector<ReflectedConstantBlock>& blocks) {
    const auto appendTypeFields = [&hlsl](
                                      auto&& self,
                                      render::HlslShaderTypeId typeId,
                                      uint32_t baseOffset,
                                      std::string_view prefix,
                                      vector<ShaderConstantFieldPlan>& fields) -> void {
        if (static_cast<size_t>(typeId) >= hlsl.Types.size()) {
            return;
        }
        const render::HlslShaderTypeDesc& type = hlsl.Types[static_cast<size_t>(typeId)];
        if (type.Members.empty()) {
            const size_t reflectedSize = type.GetSizeInBytes();
            if (!prefix.empty() && reflectedSize > 0 &&
                reflectedSize <= std::numeric_limits<uint32_t>::max()) {
                fields.push_back(ShaderConstantFieldPlan{
                    .Name = string{prefix},
                    .Offset = baseOffset + type.Offset,
                    .Size = static_cast<uint32_t>(reflectedSize)});
            }
            return;
        }
        for (const render::HlslShaderTypeMember& member : type.Members) {
            if (static_cast<size_t>(member.Type) >= hlsl.Types.size()) {
                continue;
            }
            const render::HlslShaderTypeDesc& memberType =
                hlsl.Types[static_cast<size_t>(member.Type)];
            const string memberName = prefix.empty()
                                          ? member.Name
                                          : fmt::format("{}.{}", prefix, member.Name);
            if (!memberType.Members.empty() && memberType.Elements == 0) {
                self(
                    self,
                    member.Type,
                    baseOffset + memberType.Offset,
                    memberName,
                    fields);
                continue;
            }
            const size_t reflectedSize = memberType.GetSizeInBytes();
            if (reflectedSize == 0 || reflectedSize > std::numeric_limits<uint32_t>::max()) {
                continue;
            }
            fields.push_back(ShaderConstantFieldPlan{
                .Name = memberName,
                .Offset = baseOffset + memberType.Offset,
                .Size = static_cast<uint32_t>(reflectedSize)});
        }
    };

    for (const render::HlslShaderBufferDesc& cb : hlsl.ConstantBuffers) {
        if (cb.Type != render::HlslCBufferType::CBUFFER) {
            continue;
        }
        const auto resource = std::ranges::find_if(
            hlsl.BoundResources,
            [&cb](const render::HlslInputBindDesc& binding) noexcept {
                return binding.Name == cb.Name &&
                       binding.Type == render::HlslShaderInputType::CBUFFER;
            });
        if (resource == hlsl.BoundResources.end()) {
            continue;
        }
        const render::ShaderBindingLocation location{
            .Group = resource->VkSet.value_or(resource->Space),
            .Binding = resource->VkBinding.value_or(resource->BindPoint)};
        if (std::ranges::any_of(blocks, [&](const auto& block) { return block.Location == location; })) {
            continue;
        }
        ReflectedConstantBlock block{.Name = cb.Name, .Location = location, .Size = cb.Size};
        for (const size_t variableIndex : cb.Variables) {
            if (variableIndex >= hlsl.Variables.size()) {
                continue;
            }
            const render::HlslShaderVariableDesc& variable = hlsl.Variables[variableIndex];
            if (static_cast<size_t>(variable.Type) < hlsl.Types.size() &&
                !hlsl.Types[static_cast<size_t>(variable.Type)].Members.empty()) {
                appendTypeFields(
                    appendTypeFields,
                    variable.Type,
                    variable.StartOffset,
                    {},
                    block.Fields);
                continue;
            }
            block.Fields.push_back(ShaderConstantFieldPlan{
                .Name = variable.Name,
                .Offset = variable.StartOffset,
                .Size = variable.Size});
        }
        blocks.push_back(std::move(block));
    }
}

void AppendSpirvBlocks(
    const render::SpirvShaderDesc& spirv,
    vector<ReflectedConstantBlock>& blocks) {
    for (const render::SpirvResourceBinding& resource : spirv.ResourceBindings) {
        if (resource.Kind != render::SpirvResourceKind::UniformBuffer) {
            continue;
        }
        const render::ShaderBindingLocation location{
            .Group = resource.Set,
            .Binding = resource.Binding};
        if (std::ranges::any_of(blocks, [&](const auto& block) { return block.Location == location; })) {
            continue;
        }
        ReflectedConstantBlock block{
            .Name = resource.Name,
            .Location = location,
            .Size = resource.UniformBufferSize};
        if (resource.TypeIndex < spirv.Types.size()) {
            for (const render::SpirvTypeMember& member : spirv.Types[resource.TypeIndex].Members) {
                block.Fields.push_back(ShaderConstantFieldPlan{
                    .Name = member.Name,
                    .Offset = member.Offset,
                    .Size = member.Size});
            }
        }
        blocks.push_back(std::move(block));
    }
}

void AppendShaderBlocks(
    render::Shader* shader,
    vector<ReflectedConstantBlock>& blocks) {
    if (shader == nullptr) {
        return;
    }
    auto reflection = shader->GetReflection();
    if (!reflection.HasValue() || reflection.Get() == nullptr) {
        return;
    }
    if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(reflection.Get())) {
        AppendHlslBlocks(*hlsl, blocks);
    } else if (const auto* spirv = std::get_if<render::SpirvShaderDesc>(reflection.Get())) {
        AppendSpirvBlocks(*spirv, blocks);
    }
}

std::optional<ShaderBindingEntryKind> ClassifyEntry(
    const render::BindingGroupLayoutEntry& entry) noexcept {
    if (entry.IsStaticSampler) {
        return ShaderBindingEntryKind::StaticSampler;
    }
    if (entry.Parameter.Count != 1 || entry.Parameter.IsBindless) {
        return std::nullopt;
    }
    if (entry.Parameter.Kind == render::ShaderParameterKind::Resource &&
        entry.Parameter.Type == render::ResourceBindType::CBuffer) {
        return ShaderBindingEntryKind::ConstantBuffer;
    }
    if (entry.Parameter.Kind == render::ShaderParameterKind::Resource &&
        entry.Parameter.Type == render::ResourceBindType::Texture &&
        entry.Parameter.IsReadOnly) {
        return ShaderBindingEntryKind::Texture;
    }
    if (entry.Parameter.Kind == render::ShaderParameterKind::Sampler &&
        entry.Parameter.Type == render::ResourceBindType::Sampler) {
        return ShaderBindingEntryKind::Sampler;
    }
    return std::nullopt;
}

unique_ptr<ShaderBindingPlan> BuildPlan(
    const ShaderAsset& shader,
    uint32_t passIndex,
    const CompiledShaderVariant& variant) {
    auto plan = make_unique<ShaderBindingPlan>();
    plan->ProgramId = shader.GetProgramId();
    plan->PassIndex = passIndex;
    if (passIndex >= shader.GetPasses().size() || variant.Layout == nullptr) {
        plan->Error = "shader pass or compiled layout is unavailable";
        return plan;
    }
    if (!variant.Layout->GetPushConstantRanges().empty()) {
        plan->Error = "material push constants are not supported by the generic mesh binder";
        return plan;
    }

    const ShaderPassDesc& pass = shader.GetPasses()[passIndex];
    vector<ReflectedConstantBlock> blocks;
    AppendShaderBlocks(variant.VS, blocks);
    AppendShaderBlocks(variant.PS, blocks);
    AppendShaderBlocks(variant.CS, blocks);

    vector<render::BindingGroupLayout> layouts = variant.Layout->GetBindingGroupLayouts();
    std::ranges::sort(layouts, {}, &render::BindingGroupLayout::GroupIndex);
    for (render::BindingGroupLayout& layout : layouts) {
        std::ranges::sort(layout.Entries, {}, &render::BindingGroupLayoutEntry::Binding);
        ShaderBindingGroupPlan group{.Group = layout.GroupIndex};
        for (const render::BindingGroupLayoutEntry& reflected : layout.Entries) {
            const auto kind = ClassifyEntry(reflected);
            if (!kind.has_value()) {
                plan->Error = fmt::format(
                    "unsupported parameter '{}' at group {} binding {}",
                    reflected.Parameter.Name,
                    layout.GroupIndex,
                    reflected.Binding);
                plan->ErrorGroup = layout.GroupIndex;
                plan->ErrorBinding = reflected.Binding;
                return plan;
            }
            ShaderBindingEntryPlan entry{
                .Name = reflected.Parameter.Name,
                .Group = layout.GroupIndex,
                .Binding = reflected.Binding,
                .Count = reflected.Parameter.Count,
                .ByteSize = reflected.Parameter.ByteSize,
                .ParameterKind = reflected.Parameter.Kind,
                .ResourceType = reflected.Parameter.Type,
                .Kind = *kind,
                .HasDynamicOffset = reflected.HasDynamicOffset};

            const ShaderParameterSourceDesc* wholeSource = FindSource(pass, {}, entry.Name);
            entry.Source = MakeSource(wholeSource, entry.Name);
            entry.Frequency = MapFrequency(entry.Source.Scope);

            if (entry.Kind == ShaderBindingEntryKind::ConstantBuffer) {
                const auto blockIt = std::ranges::find_if(
                    blocks,
                    [&](const ReflectedConstantBlock& block) {
                        return block.Location.Group == entry.Group &&
                               block.Location.Binding == entry.Binding;
                    });
                if (blockIt == blocks.end() || blockIt->Size == 0) {
                    plan->Error = fmt::format("cbuffer '{}' has no usable reflection", entry.Name);
                    plan->ErrorGroup = entry.Group;
                    plan->ErrorBinding = entry.Binding;
                    return plan;
                }
                entry.ByteSize = blockIt->Size;
                entry.Fields = blockIt->Fields;
                bool hasIncompatibleFieldSource = false;
                for (ShaderConstantFieldPlan& field : entry.Fields) {
                    if (field.Size == 0 || field.Offset >= entry.ByteSize ||
                        field.Size > entry.ByteSize - field.Offset) {
                        plan->Error = fmt::format(
                            "cbuffer '{}.{}' has an invalid reflected byte range",
                            entry.Name,
                            field.Name);
                        plan->ErrorGroup = entry.Group;
                        plan->ErrorBinding = entry.Binding;
                        return plan;
                    }
                    const ShaderParameterSourceDesc* fieldSource =
                        FindSource(pass, entry.Name, field.Name);
                    if (fieldSource == wholeSource) {
                        fieldSource = nullptr;
                    }
                    field.Source = MakeSource(fieldSource, field.Name);
                    if (wholeSource != nullptr && field.Source.Explicit &&
                        (wholeSource->Scope != ShaderParameterScope::Material ||
                         field.Source.Scope != ShaderParameterScope::Material)) {
                        hasIncompatibleFieldSource = true;
                    }
                    entry.Frequency = MaxFrequency(
                        entry.Frequency,
                        MapFrequency(field.Source.Scope));
                }
                if (hasIncompatibleFieldSource) {
                    plan->Error = fmt::format(
                        "cbuffer '{}' mixes an incompatible whole-block and field source",
                        entry.Name);
                    plan->ErrorGroup = entry.Group;
                    plan->ErrorBinding = entry.Binding;
                    return plan;
                }
                if (wholeSource == nullptr) {
                    entry.Source = ShaderBindingSource{
                        .Scope = ShaderParameterScope::Material,
                        .ProviderName = entry.Name,
                        .Explicit = false};
                    entry.Frequency = ShaderBindingFrequency::Material;
                    for (const ShaderConstantFieldPlan& field : entry.Fields) {
                        entry.Frequency = MaxFrequency(
                            entry.Frequency,
                            MapFrequency(field.Source.Scope));
                    }
                }
            }

            const uint32_t entryIndex = static_cast<uint32_t>(plan->Entries.size());
            group.EntryIndices.push_back(entryIndex);
            if (entry.HasDynamicOffset) {
                group.DynamicEntryIndices.push_back(entryIndex);
            }
            group.Frequency = MaxFrequency(group.Frequency, entry.Frequency);
            plan->Entries.push_back(std::move(entry));
        }
        if (!group.EntryIndices.empty()) {
            plan->Groups.push_back(std::move(group));
        }
    }
    plan->Valid = true;
    return plan;
}

}  // namespace

Nullable<const ShaderBindingPlan*> ShaderBindingPlanLibrary::GetOrCreate(
    const ShaderAsset& shader,
    uint32_t passIndex,
    const CompiledShaderVariant& variant) noexcept {
    for (const Entry& entry : _entries) {
        if (entry.ProgramId == shader.GetProgramId() &&
            entry.PassIndex == passIndex && entry.VariantKey == variant.Key) {
            ++_hits;
            return entry.Plan.get();
        }
    }
    ++_misses;
    auto plan = BuildPlan(shader, passIndex, variant);
    const ShaderBindingPlan* result = plan.get();
    _entries.push_back(Entry{
        .ProgramId = shader.GetProgramId(),
        .PassIndex = passIndex,
        .VariantKey = variant.Key,
        .Plan = std::move(plan)});
    return result;
}

void ShaderBindingPlanLibrary::Clear() noexcept {
    _entries.clear();
}

}  // namespace radray

#include <radray/runtime/render_framework/material_template.h>

#include <algorithm>
#include <radray/runtime/render_system.h>

namespace radray {
std::optional<CompiledMaterialTemplate> CompiledMaterialTemplate::Compile(render::Device* device, RenderSystem& services,
                                                                          const MaterialTemplateDescriptor& descriptor, string& error) {
    const auto fail = [&](std::string_view reason) -> std::optional<CompiledMaterialTemplate> {
        error = fmt::format("Material template '{}': {}", descriptor.Name, reason);
        return std::nullopt;
    };
    if (descriptor.Name.empty() || (descriptor.Program.SourceName.empty() == !descriptor.RegisteredProgram))
        return fail("requires a name and exactly one program source");
    auto program = descriptor.RegisteredProgram ? services.ResolveShaderProgram(*descriptor.RegisteredProgram) : services.GetOrCreateShaderProgram(descriptor.Program);
    if (!program || program->GetDevice() != device) return fail("program is unavailable or belongs to another device");
    if (!descriptor.ObjectIndexPushConstant.empty() && !descriptor.PushConstant.empty()) return fail("multiple push blocks");
    CompiledMaterialTemplate result{descriptor, program.Get()};
    const auto& artifact = program->GetArtifact().Generic();
    const auto pushName = descriptor.PushConstant.empty() ? descriptor.ObjectIndexPushConstant : descriptor.PushConstant;
    if (!pushName.empty()) {
        result.Push = program->GetPipelineLayout()->FindBinding(pushName);
        const auto push = std::find_if(artifact.RootConstants().begin(), artifact.RootConstants().end(), [&](const auto& block) { return artifact.GetName(block.Name) == pushName; });
        if (!result.Push.IsValid() || push == artifact.RootConstants().end() || artifact.RootConstants().size() != 1)
            return fail("push block does not match the artifact");
        result.PushSize = push->Size;
        if (!descriptor.ObjectIndexPushConstant.empty()) {
            if (push->Size != 4 || descriptor.InstanceAddressing != MaterialInstanceAddressing::DirectObject || !descriptor.SceneIndexField.empty())
                return fail("object index push must be one uint with direct addressing");
            result.SceneIndexOffset = 0;
        } else if (!descriptor.SceneIndexField.empty()) {
            const auto field = std::find_if(artifact.Types().begin(), artifact.Types().end(), [&](const auto& type) {
                return type.ParentIndex == push->TypeIndex && artifact.GetName(type.Name) == descriptor.SceneIndexField;
            });
            if (field == artifact.Types().end() || field->Size != 4 || field->ScalarKind != uint32_t(shader::ShaderScalarKind::UnsignedInteger) ||
                field->RowCount != 1 || field->ColumnCount != 1 || field->Offset > push->Size || 4 > push->Size - field->Offset)
                return fail("scene index must name a reflected scalar uint");
            result.SceneIndexOffset = field->Offset;
        }
        if (!descriptor.ScenePushValues.empty() && descriptor.ScenePushValues.size() != push->Size) return fail("push default size differs from reflection");
    } else if (!artifact.RootConstants().empty() || !descriptor.ScenePushValues.empty())
        return fail("push definition is missing");
    const auto needsConstant = [](const render::BlendComponent& blend) {
        return blend.Src == render::BlendFactor::Constant || blend.Src == render::BlendFactor::OneMinusConstant ||
               blend.Dst == render::BlendFactor::Constant || blend.Dst == render::BlendFactor::OneMinusConstant;
    };
    if (descriptor.Blend && (needsConstant(descriptor.Blend->Color) || needsConstant(descriptor.Blend->Alpha))) return fail("constant blend state requires an explicit recording contract");
    if (descriptor.DepthStencil && descriptor.DepthStencil->Stencil) return fail("stencil state requires an explicit recording contract");
    for (uint32_t i = 0; i < descriptor.Inputs.size(); ++i) {
        const auto& input = descriptor.Inputs[i];
        if (input.Name.empty() || std::any_of(descriptor.Inputs.begin(), descriptor.Inputs.begin() + i, [&](const auto& other) { return other.Name == input.Name; }))
            return fail("duplicate or empty input name");
        const auto binding = program->GetArtifact().FindBindingInfo(input.Name);
        if (!binding) return fail(fmt::format("input '{}' is absent from the artifact", input.Name));
        const auto kind = binding->LogicalKind;
        using Kind = shader::ShaderBindingKind;
        MaterialBindingDomain domain;
        bool valid;
        switch (input.Source) {
            case MaterialInputSource::SceneObjects:
                domain = MaterialBindingDomain::SceneFrame;
                valid = kind == Kind::StructuredBuffer && binding->Count == 1;
                break;
            case MaterialInputSource::InstanceParameters:
                domain = MaterialBindingDomain::SceneFrame;
                valid = kind == Kind::StructuredBuffer && binding->Count == 1 && input.ValueIndex < descriptor.ParameterLayouts.size() && descriptor.ParameterLayouts[input.ValueIndex].IsValid();
                break;
            case MaterialInputSource::FrameConstants:
                domain = MaterialBindingDomain::SceneFrame;
                valid = kind == Kind::CBuffer;
                break;
            case MaterialInputSource::ViewConstants:
                domain = MaterialBindingDomain::ViewPass;
                valid = kind == Kind::CBuffer;
                break;
            case MaterialInputSource::VisibleInstanceSlots:
                domain = MaterialBindingDomain::ViewPass;
                valid = kind == Kind::StructuredBuffer && binding->Count == 1;
                break;
            case MaterialInputSource::MaterialConstants:
                domain = MaterialBindingDomain::Material;
                valid = kind == Kind::CBuffer;
                break;
            case MaterialInputSource::Texture:
                domain = MaterialBindingDomain::Material;
                valid = kind == Kind::Texture;
                break;
            case MaterialInputSource::Sampler:
                domain = MaterialBindingDomain::Sampler;
                valid = kind == Kind::Sampler;
                break;
            default: return fail("unknown input source");
        }
        if (!valid || !binding->Count || input.ValueIndex > UINT32_MAX - binding->Count) return fail("input kind, array or instance schema differs from reflection");
        const auto handle = program->GetPipelineLayout()->FindBinding(input.Name);
        if (!handle.IsValid()) return fail("binding handle could not be resolved");
        uint32_t size = 0;
        if (kind == Kind::CBuffer) {
            const auto declaration = artifact.FindBinding(input.Name);
            if (!declaration || declaration->Record.TypeIndex >= artifact.Types().size()) return fail("constant buffer reflection is incomplete");
            size = artifact.Types()[declaration->Record.TypeIndex].Size;
        }
        result.Inputs.push_back({input, handle, *binding, size});
        auto group = std::find_if(result.Groups.begin(), result.Groups.end(), [&](const auto& value) { return value.Index == binding->Group; });
        if (group == result.Groups.end()) {
            result.Groups.push_back({binding->Group, domain, {}});
            group = result.Groups.end() - 1;
        } else if (group->Domain != domain)
            return fail(fmt::format("group {} mixes binding domains at '{}'; split the shader group", binding->Group, input.Name));
        if (!binding->Immutable) group->Inputs.push_back(i);
    }
    for (const auto& binding : artifact.Bindings()) {
        const auto name = artifact.GetName(binding.Name);
        if (!name) return fail("unnamed artifact binding");
        const auto info = program->GetArtifact().FindBindingInfo(*name);
        if (!info) return fail("unresolved artifact binding");
        if (info->Immutable) {
            const auto group = std::find_if(result.Groups.begin(), result.Groups.end(), [&](const auto& value) { return value.Index == info->Group; });
            if (group == result.Groups.end())
                result.Groups.push_back({info->Group, MaterialBindingDomain::Sampler, {}});
            else if (group->Domain != MaterialBindingDomain::Sampler)
                return fail(fmt::format("group {} mixes an immutable sampler with another binding domain", info->Group));
        }
        if (!info->Immutable && std::none_of(descriptor.Inputs.begin(), descriptor.Inputs.end(), [&](const auto& input) { return input.Name == *name; }))
            return fail(fmt::format("active binding '{}' has no input source", *name));
    }
    for (const auto& group : result.Groups)
        if (!program->GetPipelineLayout()->GetParameterGroupCompatibility(group.Index)) return fail("group has no backend compatibility identity");
    if (descriptor.InstanceAddressing == MaterialInstanceAddressing::VisibleSlots &&
        (!result.SceneIndexOffset || std::none_of(descriptor.Inputs.begin(), descriptor.Inputs.end(), [](const auto& input) { return input.Source == MaterialInputSource::VisibleInstanceSlots; })))
        return fail("visible-slot addressing requires both a visible list and reflected scene index");
    error.clear();
    return result;
}
bool CompiledMaterialTemplate::Equivalent(const CompiledMaterialTemplate& other) const noexcept {
    const auto& a = Descriptor;
    const auto& b = other.Descriptor;
    return Program == other.Program && a.Name == b.Name && a.Inputs == b.Inputs && a.ObjectIndexPushConstant == b.ObjectIndexPushConstant &&
           a.Primitive == b.Primitive && a.Blend == b.Blend && a.WriteMask == b.WriteMask && a.DepthStencil == b.DepthStencil && a.Samples == b.Samples &&
           a.PushConstant == b.PushConstant && a.ParameterLayouts == b.ParameterLayouts && a.InstanceAddressing == b.InstanceAddressing &&
           a.AllowAutomaticInstancing == b.AllowAutomaticInstancing && a.SceneIndexField == b.SceneIndexField && a.ScenePushValues == b.ScenePushValues && a.PreserveOrder == b.PreserveOrder;
}
}  // namespace radray

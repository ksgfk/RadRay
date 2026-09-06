#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>

#include <algorithm>
#include <functional>

namespace radray {

std::optional<RendererListPassBindings> RendererListPassBindings::Build(
    RenderGraphRasterBuilder& builder, const RendererList& list, std::span<const RendererListPassBinding> bindings) {
    const auto fail = [&](std::string_view code, std::string_view message, uint32_t group) -> std::optional<RendererListPassBindings> {
        builder.Reject(code, message, fmt::format("group {}", group));
        return std::nullopt;
    };
    for (size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (!binding.Program || !builder.OwnsParameterSet(binding.Parameters, *binding.Program, binding.Group))
            return fail("RendererListParameterScope", "Parameter set belongs to a different graph, pass, program or group", binding.Group);
        if (std::none_of(list.Commands.begin(), list.Commands.end(), [&](const auto& draw) { return draw.Program.Get() == binding.Program; }))
            return fail("RendererListProgram", "Parameter program is not used by this renderer list", binding.Group);
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (bindings[earlier].Program == binding.Program && bindings[earlier].Group == binding.Group)
                return fail("RendererListGroupCollision", "A program group has more than one graph parameter set", binding.Group);
    }
    for (const auto& draw : list.Commands) {
        if (!draw.Program) return fail("RendererListProgram", "Draw has no shader program", 0);
        for (size_t index = 0; index < draw.Groups.size(); ++index) {
            const auto& native = draw.Groups[index];
            if (!native.Set || (index && draw.Groups[index - 1].Group >= native.Group))
                return fail("RendererListNativeGroup", "Native groups must be valid, sorted and unique", native.Group);
            for (const auto& binding : bindings)
                if (binding.Program == draw.Program.Get() && binding.Group == native.Group)
                    return fail("RendererListGroupCollision", "Native and graph parameter sets collide for this program", native.Group);
        }
        const auto& artifact = draw.Program->GetArtifact().Generic();
        for (const auto& declaration : artifact.Bindings()) {
            const auto name = artifact.GetName(declaration.Name);
            if (!name) continue;
            const auto info = draw.Program->GetArtifact().FindBindingInfo(*name);
            if (!info || info->Immutable) continue;
            const bool native = std::any_of(draw.Groups.begin(), draw.Groups.end(), [&](const auto& value) { return value.Group == info->Group; });
            const bool graph = std::any_of(bindings.begin(), bindings.end(), [&](const auto& value) { return value.Program == draw.Program.Get() && value.Group == info->Group; });
            if (!native && !graph) {
                builder.Reject("RendererListMissingGroup", "Draw is missing a required native or graph parameter group", *name);
                return std::nullopt;
            }
        }
    }
    RendererListPassBindings result;
    result._pass = builder.GetPassHandle();
    for (const auto& draw : list.Commands)
        if (std::find(result._programs.begin(), result._programs.end(), draw.Program.Get()) == result._programs.end())
            result._programs.push_back(draw.Program.Get());
    result._bindings.assign(bindings.begin(), bindings.end());
    std::sort(result._bindings.begin(), result._bindings.end(), [](const auto& left, const auto& right) {
        return left.Program == right.Program ? left.Group < right.Group : std::less<ShaderProgram*>{}(left.Program, right.Program);
    });
    return result;
}

std::optional<RendererListPassBindings> RendererListPassBindings::Create(
    RenderGraphRasterBuilder& builder, const RendererList& list, std::span<const RendererListProgramParameters> parameters) {
    vector<RendererListPassBinding> bindings;
    bindings.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (!parameter.Program) {
            builder.Reject("RendererListProgram", "Graph group requires a shader program");
            return std::nullopt;
        }
        const auto handle = builder.CreateParameterSet(*parameter.Program, parameter.Group, parameter.Bindings);
        if (!handle.IsValid()) return std::nullopt;
        bindings.push_back({parameter.Program, parameter.Group, handle});
    }
    return Build(builder, list, bindings);
}

std::span<const RendererListPassBinding> RendererListPassBindings::Find(const ShaderProgram& program) const noexcept {
    const auto first = std::find_if(_bindings.begin(), _bindings.end(), [&](const auto& value) { return value.Program == &program; });
    const auto end = std::find_if(first, _bindings.end(), [&](const auto& value) { return value.Program != &program; });
    return {first, end};
}

bool RendererListPassBindings::IsValidFor(const RenderGraphRasterContext& context, const ShaderProgram& program) const noexcept {
    if (_pass != context.GetPassHandle() || std::find(_programs.begin(), _programs.end(), &program) == _programs.end()) return false;
    for (const auto& binding : Find(program))
        if (!context.OwnsParameterSet(binding.Parameters, program, binding.Group)) return false;
    return true;
}

}  // namespace radray

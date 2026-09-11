#include <radray/runtime/render_framework/renderer_list_pass_sets.h>

#include <algorithm>
#include <functional>
#include <numeric>
#include <radray/profiler.h>

namespace radray {
namespace {

struct GraphGroup {
    const ShaderProgram* Program;
    uint32_t Group;
};

// Full only: these scans are per draw. A Performance build leaves a missing or colliding group to
// the backend validation layer instead of paying for the scan every frame.
bool ValidateDrawGroups(RenderGraphPrepareContext& ctx, const RendererList& list, std::span<const GraphGroup> groups) {
    RADRAY_PROFILE_SCOPE_N("ValidatePassSets");
    const auto fail = [&](std::string_view code, std::string_view message, std::string_view binding) {
        ctx.Reject(code, message, binding);
        return false;
    };
    unordered_map<const ShaderProgram*, vector<std::pair<uint32_t, std::string_view>>> requiredByProgram;
    for (const auto& draw : list.Commands) {
        if (!draw.Program) continue;
        for (size_t index = 0; index < draw.Groups.size(); ++index) {
            const auto& native = draw.Groups[index];
            if (!native.Set || (index && draw.Groups[index - 1].Group >= native.Group))
                return fail("RendererListNativeGroup", "Native groups must be valid, sorted and unique",
                            fmt::format("group {}", native.Group));
            for (const auto& group : groups)
                if (group.Program == draw.Program.Get() && group.Group == native.Group)
                    return fail("RendererListGroupCollision", "Native and graph parameter sets collide for this program",
                                fmt::format("group {}", native.Group));
        }
        auto [requirements, inserted] = requiredByProgram.try_emplace(draw.Program.Get());
        if (inserted) {
            const auto& artifact = draw.Program->GetArtifact().Generic();
            for (const auto& declaration : artifact.Bindings()) {
                const auto name = artifact.GetName(declaration.Name);
                if (!name) continue;
                const auto info = draw.Program->GetArtifact().FindBindingInfo(*name);
                if (!info || info->Immutable) continue;
                requirements->second.emplace_back(info->Group, *name);
            }
        }
        for (const auto& [group, name] : requirements->second) {
            const bool native = std::any_of(draw.Groups.begin(), draw.Groups.end(),
                                            [&](const PreparedShaderGroup& value) { return value.Group == group; });
            const bool graph = std::any_of(groups.begin(), groups.end(), [&](const GraphGroup& value) {
                return value.Program == draw.Program.Get() && value.Group == group;
            });
            if (!native && !graph)
                return fail("RendererListMissingGroup", "Draw is missing a required native or graph parameter group", name);
        }
    }
    return true;
}

}  // namespace

std::optional<RendererListPassSets> RendererListPassSets::Create(
    RenderGraphPrepareContext& ctx, const RendererList& list, std::span<const RendererListProgramParameters> parameters) {
    RADRAY_PROFILE_SCOPE_N("CreatePassSets");
    const auto fail = [&](std::string_view code, std::string_view message, uint32_t group) -> std::optional<RendererListPassSets> {
        ctx.Reject(code, message, fmt::format("group {}", group));
        return std::nullopt;
    };
    // Argument checks run over a handful of entries, so they stay unconditional.
    vector<GraphGroup> groups;
    groups.reserve(parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (!parameter.Program) return fail("RendererListProgram", "Graph group requires a shader program", parameter.Group);
        if (std::none_of(list.Commands.begin(), list.Commands.end(),
                         [&](const MeshDrawCommand& draw) { return draw.Program.Get() == parameter.Program; }))
            return fail("RendererListProgram", "Parameter program is not used by this renderer list", parameter.Group);
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (parameters[earlier].Program == parameter.Program && parameters[earlier].Group == parameter.Group)
                return fail("RendererListGroupCollision", "A program group has more than one graph parameter set", parameter.Group);
        groups.push_back({parameter.Program, parameter.Group});
    }
    if (ctx.IsValidationFull() && !ValidateDrawGroups(ctx, list, groups)) return std::nullopt;

    // Sets are created in the caller's order so constant uploads keep a predictable arena order;
    // the lookup index is sorted afterwards.
    vector<PreparedShaderGroup> created;
    created.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        const PreparedShaderGroup set = ctx.CreateParameterSet(*parameter.Program, parameter.Group, parameter.Bindings);
        if (!set.Set) return std::nullopt;
        created.push_back(set);
    }
    vector<uint32_t> order(parameters.size());
    std::iota(order.begin(), order.end(), uint32_t{0});
    std::stable_sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
        if (parameters[left].Program != parameters[right].Program)
            return std::less<const ShaderProgram*>{}(parameters[left].Program, parameters[right].Program);
        return parameters[left].Group < parameters[right].Group;
    });
    RendererListPassSets result;
    result._pass = ctx.GetPassHandle();
    result._sets.reserve(order.size());
    result._programs.reserve(order.size());
    for (const uint32_t index : order) {
        const ShaderProgram* program = parameters[index].Program;
        if (result._programs.empty() || result._programs.back().Program != program)
            result._programs.push_back({program, static_cast<uint32_t>(result._sets.size()), 0});
        ++result._programs.back().Count;
        result._sets.push_back(created[index]);
    }
    return result;
}

std::span<const PreparedShaderGroup> RendererListPassSets::Find(const ShaderProgram& program) const noexcept {
    for (const Range& range : _programs)
        if (range.Program == &program) return {_sets.data() + range.First, range.Count};
    return {};
}

}  // namespace radray

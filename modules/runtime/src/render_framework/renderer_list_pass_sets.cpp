#include <radray/runtime/render_framework/renderer_list_pass_sets.h>

#include <algorithm>
#include <functional>
#include <numeric>
#include <radray/profiler.h>
#include <radray/scope_guard.h>

#include "renderer_list_validation.h"

namespace radray {
namespace {

struct GraphGroup {
    const ShaderProgram* Program;
    uint32_t Group;
};

struct PassSetsValidation {
    RendererListValidationSource Source;
    vector<GraphGroup> Groups;
};

bool ValidateDrawOrder(RenderGraphPrepareContext& ctx, const RendererList& list) {
    if (list.Items.empty()) return true;
    if (list.Items.size() != list.GetDrawCount()) {
        ctx.Reject("RendererListPreparation", "Draw order must reference every command exactly once");
        return false;
    }
    vector<bool> visited(list.GetDrawCount());
    for (const auto& item : list.Items) {
        if (item.CommandIndex >= visited.size() || visited[item.CommandIndex]) {
            ctx.Reject("RendererListPreparation", "Draw order contains an invalid or duplicate command index");
            return false;
        }
        visited[item.CommandIndex] = true;
    }
    return true;
}

bool ValidateDrawGroups(const void* payload, RenderGraphPrepareContext& ctx) {
    RADRAY_PROFILE_SCOPE_N("ValidatePassSets");
    const auto& validation = *static_cast<const PassSetsValidation*>(payload);
    if (!validation.Source.IsCurrent()) {
        ctx.Reject("RendererListLifetime", "Renderer list changed before ready frame validation");
        return false;
    }
    const auto& list = *validation.Source.List;
    const auto groups = std::span<const GraphGroup>{validation.Groups};
    if (!ValidateDrawOrder(ctx, list)) return false;
    const auto fail = [&](std::string_view code, std::string_view message, std::string_view binding) {
        ctx.Reject(code, message, binding);
        return false;
    };
    unordered_map<const ShaderProgram*, vector<std::pair<uint32_t, std::string_view>>> requiredByProgram;
    for (size_t drawIndex = 0; drawIndex < list.GetDrawCount(); ++drawIndex) {
        const auto& draw = list.GetDescription(drawIndex);
        const auto nativeGroups = list.GetGroups(drawIndex);
        if (!draw.Program) continue;
        for (size_t index = 0; index < nativeGroups.size(); ++index) {
            const auto& native = nativeGroups[index];
            if (!native.Set || (index && nativeGroups[index - 1].Group >= native.Group))
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
            bool native = false;
            for (size_t index = 0; index < nativeGroups.size(); ++index)
                native |= nativeGroups[index].Group == group;
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
    RendererListPassSets result;
    if (!result.Prepare(ctx, list, parameters)) return std::nullopt;
    return result;
}

void RendererListPassSets::ResetForReuse() noexcept {
    _pass = {};
    _activeSets = 0;
    _programs.clear();
    _order.clear();
    const auto clear = [](auto& values) {
        for (auto& value : values) {
            value.Group = 0;
            value.Set = nullptr;
            value.DynamicOffsets.clear();
        }
    };
    clear(_sets);
    clear(_created);
}

size_t RendererListPassSets::GetCapacityBytes() const noexcept {
    size_t bytes = _programs.capacity() * sizeof(Range) + _order.capacity() * sizeof(uint32_t) +
                   (_sets.capacity() + _created.capacity()) * sizeof(PreparedShaderGroup);
    for (const auto& set : _sets)
        if (set.DynamicOffsets.capacity() > set.DynamicOffsets.inline_capacity) bytes += set.DynamicOffsets.capacity() * sizeof(render::ShaderParameterDynamicOffset);
    for (const auto& set : _created)
        if (set.DynamicOffsets.capacity() > set.DynamicOffsets.inline_capacity) bytes += set.DynamicOffsets.capacity() * sizeof(render::ShaderParameterDynamicOffset);
    return bytes;
}

bool RendererListPassSets::Prepare(
    RenderGraphPrepareContext& ctx, const RendererList& list, std::span<const RendererListProgramParameters> parameters) {
    RADRAY_PROFILE_SCOPE_N("CreatePassSets");
    ResetForReuse();
    auto recovery = MakeScopeGuard([this]() noexcept { ResetForReuse(); });
    if (!list.IsCurrent()) {
        ctx.Reject("RendererListLifetime", "Renderer list no longer belongs to its published snapshot and frame resources");
        return false;
    }
    if (!HasSafeRendererListOrder(list)) {
        ctx.Reject("RendererListPreparation", "Draw order exceeds its source draw array");
        return false;
    }
    const auto fail = [&](std::string_view code, std::string_view message, uint32_t group) -> bool {
        ctx.Reject(code, message, fmt::format("group {}", group));
        return false;
    };
    // Argument checks run over a handful of entries, so they stay unconditional.
    vector<GraphGroup> groups;
    if (ctx.IsValidationFull()) groups.reserve(parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (!parameter.Program) return fail("RendererListProgram", "Graph group requires a shader program", parameter.Group);
        const auto programs = list.GetPrograms();
        bool used = std::any_of(programs.begin(), programs.end(), [&](const RendererListProgramUse& use) { return use.Program.Get() == parameter.Program; });
        if (programs.empty())
            for (size_t draw = 0; draw < list.GetDrawCount() && !used; ++draw)
                used = list.GetDescription(draw).Program.Get() == parameter.Program;
        if (!used)
            return fail("RendererListProgram", "Parameter program is not used by this renderer list", parameter.Group);
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (parameters[earlier].Program == parameter.Program && parameters[earlier].Group == parameter.Group)
                return fail("RendererListGroupCollision", "A program group has more than one graph parameter set", parameter.Group);
        if (ctx.IsValidationFull()) groups.push_back({parameter.Program, parameter.Group});
    }

    // Sets are created in the caller's order so constant uploads keep a predictable arena order;
    // the lookup index is sorted afterwards.
    if (_created.size() < parameters.size()) _created.resize(parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        const PreparedShaderGroup set = ctx.CreateParameterSet(*parameter.Program, parameter.Group, parameter.Bindings);
        if (!set.Set) return false;
        auto& created = _created[index];
        created.Group = set.Group;
        created.Set = set.Set;
        created.DynamicOffsets.assign(set.DynamicOffsets.begin(), set.DynamicOffsets.end());
    }
    auto& order = _order;
    order.resize(parameters.size());
    std::iota(order.begin(), order.end(), uint32_t{0});
    std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
        if (parameters[left].Program != parameters[right].Program)
            return std::less<const ShaderProgram*>{}(parameters[left].Program, parameters[right].Program);
        return parameters[left].Group < parameters[right].Group;
    });
    _pass = ctx.GetPassHandle();
    if (_sets.size() < order.size()) _sets.resize(order.size());
    _programs.reserve(order.size());
    for (const uint32_t index : order) {
        const ShaderProgram* program = parameters[index].Program;
        if (_programs.empty() || _programs.back().Program != program)
            _programs.push_back({program, _activeSets, 0});
        ++_programs.back().Count;
        auto& set = _sets[_activeSets++];
        set.Group = _created[index].Group;
        set.Set = _created[index].Set;
        set.DynamicOffsets.assign(_created[index].DynamicOffsets.begin(), _created[index].DynamicOffsets.end());
    }
    if (ctx.IsValidationFull()) {
        ctx.DeferReadyValidation(make_shared<PassSetsValidation>(PassSetsValidation{RendererListValidationSource{list}, std::move(groups)}), ValidateDrawGroups);
    }
    recovery.Dismiss();
    return true;
}

std::span<const PreparedShaderGroup> RendererListPassSets::Find(const ShaderProgram& program) const noexcept {
    for (const Range& range : _programs)
        if (range.Program == &program) return {_sets.data() + range.First, range.Count};
    return {};
}

}  // namespace radray

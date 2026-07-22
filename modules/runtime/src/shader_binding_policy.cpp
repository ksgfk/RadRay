#include <radray/runtime/shader_binding_policy.h>

#include <algorithm>

#include <fmt/format.h>

namespace radray {
namespace {

uint32_t GetSchemaBindingIndex(const ShaderBindingProviderSchemaEntry& entry) noexcept {
    return entry.AcceptedBindings.empty() ? 0 : entry.AcceptedBindings.front().BindingIndex;
}

}  // namespace

PipelineBindingPolicy::PipelineBindingPolicy(
    vector<PipelineBindingReservation> reservations) noexcept
    : _reservations(std::move(reservations)) {
    std::ranges::sort(_reservations, {}, &PipelineBindingReservation::GroupIndex);
}

bool PipelineBindingPolicy::IsValid() const noexcept {
    for (size_t i = 0; i < _reservations.size(); ++i) {
        if (_reservations[i].Provider == nullptr || _reservations[i].Provider->GetName().empty()) return false;
        if (i != 0 && _reservations[i - 1].GroupIndex == _reservations[i].GroupIndex) return false;
    }
    return true;
}

const PipelineBindingReservation* PipelineBindingPolicy::Find(uint32_t groupIndex) const noexcept {
    const auto it = std::ranges::lower_bound(
        _reservations,
        groupIndex,
        {},
        &PipelineBindingReservation::GroupIndex);
    return it != _reservations.end() && it->GroupIndex == groupIndex ? &*it : nullptr;
}

ShaderBindingResolutionResult ResolveShaderBindings(
    const render::ShaderInterfaceDesc& interface,
    const PipelineBindingPolicy& policy,
    const render::ShaderDiagnosticContext& context) {
    ShaderBindingResolutionResult result;
    if (!render::IsShaderInterfaceValid(interface)) {
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidInterface,
            .Message = "shader binding resolution requires a valid canonical interface",
            .Context = context,
            .ProviderName = {}});
        return result;
    }
    if (!policy.IsValid()) {
        result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
            .Code = ShaderBindingDiagnosticCode::InvalidPolicy,
            .Message = "pipeline binding policy contains a null or duplicate group reservation",
            .Context = context,
            .ProviderName = {}});
        return result;
    }

    ResolvedShaderBindingPlan plan;
    plan.InterfaceHash = render::HashShaderInterface(interface);
    for (const render::ShaderBindingGroupInterfaceDesc& group : interface.BindingGroups) {
        const PipelineBindingReservation* reservation = policy.Find(group.GroupIndex);
        if (reservation == nullptr) {
            plan.UserGroups.emplace_back(group);
            continue;
        }
        ShaderBindingProviderMatchResult match = reservation->Provider->Match(group);
        if (!match.Compatible) {
            render::ShaderDiagnosticContext diagnosticContext = context;
            diagnosticContext.Group = group.GroupIndex;
            diagnosticContext.Binding = match.Binding;
            result.Diagnostics.emplace_back(ShaderBindingDiagnostic{
                .Code = ShaderBindingDiagnosticCode::ProviderRejectedGroup,
                .Message = fmt::format(
                    "provider '{}' rejected reserved binding group {}: {}",
                    reservation->Provider->GetName(),
                    group.GroupIndex,
                    match.Message.empty() ? "incompatible interface" : match.Message),
                .Context = std::move(diagnosticContext),
                .ProviderName = string{reservation->Provider->GetName()}});
            return result;
        }
        plan.ProviderGroups.emplace_back(ResolvedProviderBindingGroup{
            .GroupIndex = group.GroupIndex,
            .Provider = reservation->Provider,
            .Interface = group});
    }
    result.Plan = std::move(plan);
    return result;
}

ShaderBindingSchemaProvider::ShaderBindingSchemaProvider(
    string name,
    vector<ShaderBindingProviderSchemaEntry> entries) noexcept
    : _name(std::move(name)), _entries(std::move(entries)) {
    std::ranges::sort(_entries, [](const auto& lhs, const auto& rhs) {
        return GetSchemaBindingIndex(lhs) < GetSchemaBindingIndex(rhs);
    });
    _valid = !_name.empty() && std::ranges::all_of(_entries, [](const auto& entry) {
        return !entry.AcceptedBindings.empty() &&
               std::ranges::all_of(entry.AcceptedBindings, [&](const auto& binding) {
                   return binding.BindingIndex == entry.AcceptedBindings.front().BindingIndex;
               });
    });
    for (size_t i = 1; _valid && i < _entries.size(); ++i) {
        _valid = GetSchemaBindingIndex(_entries[i - 1]) != GetSchemaBindingIndex(_entries[i]);
    }
}

ShaderBindingProviderMatchResult ShaderBindingSchemaProvider::Match(
    const render::ShaderBindingGroupInterfaceDesc& group) const {
    if (!_valid) return ShaderBindingProviderMatchResult::Failure("provider schema is invalid");
    for (const render::ShaderBindingDesc& binding : group.Bindings) {
        const auto expected = std::ranges::find_if(_entries, [&](const ShaderBindingProviderSchemaEntry& entry) {
            return GetSchemaBindingIndex(entry) == binding.BindingIndex;
        });
        if (expected == _entries.end()) {
            return ShaderBindingProviderMatchResult::Failure(fmt::format(
                                                                 "binding {} ('{}') is not owned by this provider",
                                                                 binding.BindingIndex,
                                                                 binding.Name),
                                                             binding.BindingIndex);
        }
        const bool compatible = std::ranges::any_of(
            expected->AcceptedBindings,
            [&](const render::ShaderBindingDesc& schema) {
                return render::IsShaderBindingAbiProjectionOf(binding, schema);
            });
        if (!compatible) {
            return ShaderBindingProviderMatchResult::Failure(fmt::format(
                                                                 "binding {} ('{}') has an incompatible type or layout",
                                                                 binding.BindingIndex,
                                                                 binding.Name),
                                                             binding.BindingIndex);
        }
    }
    for (const ShaderBindingProviderSchemaEntry& entry : _entries) {
        if (!entry.Required) continue;
        const uint32_t bindingIndex = GetSchemaBindingIndex(entry);
        const bool present = std::ranges::any_of(group.Bindings, [&](const render::ShaderBindingDesc& binding) {
            return binding.BindingIndex == bindingIndex;
        });
        if (!present) {
            return ShaderBindingProviderMatchResult::Failure(fmt::format(
                                                                 "required binding {} is absent",
                                                                 bindingIndex),
                                                             bindingIndex);
        }
    }
    return ShaderBindingProviderMatchResult::Success();
}

}  // namespace radray

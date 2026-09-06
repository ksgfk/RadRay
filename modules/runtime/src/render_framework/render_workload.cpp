#include <radray/runtime/render_framework/render_workload.h>

#include <cmath>

namespace radray {

bool RenderWorkloadBuilder::RequestOutput(RenderOutputId id) {
    bool known = false;
    for (const auto& output : _outputs) known |= output.Id == id && id.IsValid();
    if (!known) {
        _plan.Diagnostics.push_back(fmt::format("Unknown output {}", id.Value));
        return false;
    }
    for (const auto requested : _plan.Outputs)
        if (requested == id) return true;
    _plan.Outputs.push_back(id);
    return true;
}

bool RenderWorkloadBuilder::AddViewFamily(RenderViewFamilyDesc family) {
    const auto fail = [&](std::string_view reason) {
        _plan.Diagnostics.push_back(fmt::format("Family '{}' output {}: {}", family.Name, family.Output.Value, reason));
        return false;
    };
    bool known = false;
    for (const auto& output : _outputs) known |= output.Id == family.Output && output.Id.IsValid();
    if (!known) return fail("unknown output");
    for (const auto& previous : _plan.ViewFamilies) {
        if (previous.Output == family.Output) return fail(fmt::format("already used by family '{}'", previous.Name));
    }
    if (!std::isfinite(family.RenderScale) || family.RenderScale <= 0) return fail("RenderScale must be finite and positive");
    for (const auto& view : family.Views) {
        string reason;
        if (!ValidateRenderView(view, reason)) return fail(fmt::format("View '{}': {}", view.Name, reason));
        if (view.StateId.IsValid()) {
            for (const auto& previousFamily : _plan.ViewFamilies)
                for (const auto& previous : previousFamily.Views)
                    if (previous.StateId == view.StateId) return fail("ViewStateId already used in this frame");
        }
    }
    for (size_t index = 0; index < family.Views.size(); ++index) {
        for (size_t old = 0; old < index; ++old)
            if (family.Views[index].StateId.IsValid() && family.Views[index].StateId == family.Views[old].StateId) return fail("duplicate ViewStateId in family");
    }
    RequestOutput(family.Output);
    _plan.ViewFamilies.push_back(std::move(family));
    return true;
}

void RenderWorkloadBuilder::AddPresentationOutputs() {
    for (const auto& output : _outputs) {
        if (output.Active && output.Kind == RenderOutputKind::Presentation) RequestOutput(output.Id);
    }
}

}  // namespace radray

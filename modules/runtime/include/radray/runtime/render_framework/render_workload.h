#pragma once

#include <radray/runtime/render_framework/render_view.h>

namespace radray {

struct RenderFramePlan {
    vector<RenderOutputId> Outputs;
    vector<RenderViewFamilyDesc> ViewFamilies;
    vector<string> Diagnostics;
    void Reset() noexcept {
        Outputs.clear();
        ViewFamilies.clear();
        Diagnostics.clear();
    }
};

class RenderWorkloadBuilder {
public:
    RenderWorkloadBuilder(RenderFramePlan& plan, std::span<const RenderOutputInfo> outputs) noexcept : _plan(plan), _outputs(outputs) {}
    bool AddViewFamily(RenderViewFamilyDesc family);
    bool RequestOutput(RenderOutputId output);
    void AddPresentationOutputs();

private:
    RenderFramePlan& _plan;
    std::span<const RenderOutputInfo> _outputs;
};

}  // namespace radray

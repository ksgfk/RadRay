#pragma once

#include <radray/runtime/render_framework/render_view.h>

namespace radray {

/**
本帧要画到哪个 output？
使用哪些 view？
每个 view 的相机、矩形、layer、渲染比例是什么？
 */
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

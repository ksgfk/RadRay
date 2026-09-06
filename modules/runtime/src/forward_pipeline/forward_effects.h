#pragma once

#include "forward_frame.h"
#include "forward_bindings.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>

namespace radray {
class RenderSystem;
namespace forward_detail {

struct ForwardEffectPrograms {
    array<Nullable<ShaderProgram*>, 14> Programs{};
    bool Initialize(RenderSystem& system);
};

struct ForwardViewSignature {
    RenderExtent Extent;
    Rect ViewRect;
    render::TextureFormat OutputFormat;
    ForwardPipelineSettings Settings;
    bool Matches(const ForwardViewSignature& other) const noexcept;
};

struct ForwardHdrView {
    ForwardViewDrawWork Main;
    array<ForwardViewDrawWork, 4> Cascades;
    unique_ptr<MappedUploadPage> Lights;
    RenderExternalBuffer LightImport{};
    DrawExecutionStats Execution;
    ViewCompletionToken Completion;
    bool ContentValid{false}, PassesSucceeded{true};
    void Reset();
};

bool BuildForwardHdrView(RenderGraph& graph, RenderPipelineContext& context, render::Device& device,
                         const ForwardEffectPrograms& programs, const ForwardPipelineSettings& settings,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& sourceView,
                         const RenderSceneSnapshot& scene, FrameDrawResources& draws, ForwardBindingCache& bindings,
                         ForwardHdrView& work, bool firstOutputView, bool& lightOverflowWarned);

bool BuildForwardOutputOverlay(RenderGraph& graph, RenderPipelineContext& context, const ForwardEffectPrograms& programs,
                               const ForwardOutputOverlay& overlay, render::RenderBackend backend, bool& success);

}  // namespace forward_detail
}  // namespace radray

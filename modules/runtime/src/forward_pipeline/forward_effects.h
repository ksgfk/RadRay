#pragma once

#include "forward_frame.h"
#include "forward_bindings.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>

namespace radray {
class RenderSystem;
namespace forward_detail {

class ForwardLitMeshPassProcessor;

struct ForwardEffectPrograms {
    array<Nullable<ShaderProgram*>, 15> Programs{};
    bool Initialize(RenderSystem& system);
};

struct ForwardShadowAtlas {
    RgTextureValue Texture;
    array<Eigen::Matrix4f, 4> Matrices{};
    array<Eigen::Vector4f, 4> Spheres{}, Bias{};
    Eigen::Vector4f Params{Eigen::Vector4f::Zero()};
};

struct ForwardViewSignature {
    RenderExtent Extent;
    Rect ViewRect;
    render::TextureFormat OutputFormat;
    ForwardPipelineSettings Settings;
    bool Auxiliary{false};
    bool Matches(const ForwardViewSignature& other) const noexcept;
};

struct ForwardHdrView {
    ForwardViewDrawWork Main;
    array<ForwardViewDrawWork, 4> Cascades;
    DrawExecutionStats Execution;
    ViewCompletionToken Completion;
    bool ContentValid{false}, PassesSucceeded{true};
    void Reset();
};

bool DeclareForwardSharedShadows(RenderGraph& graph, const ForwardPipelineSettings& settings, const ResolvedRenderView& primary,
                                 const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                                 ForwardHdrView& work, render::RenderBackend backend, bool& warned, ForwardShadowAtlas& out);

bool BuildForwardHdrView(RenderGraph& graph, RenderPipelineContext& context, render::Device& device,
                         const ForwardEffectPrograms& programs, const ForwardPipelineSettings& settings,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& sourceView,
                         const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                         ForwardHdrView& work, bool firstOutputView, bool& lightOverflowWarned,
                         std::span<const ForwardOutputSurface> surfaces, std::span<RenderGraphOutputBinding> outputs,
                         const ForwardShadowAtlas& shadows, bool auxiliary, ForwardLitMeshPassProcessor* sharedLit = nullptr);

bool BuildForwardOutputOverlay(RenderGraph& graph, RenderPipelineContext& context, const ForwardEffectPrograms& programs,
                               const ForwardOutputOverlay& overlay, render::RenderBackend backend, std::span<RenderGraphOutputBinding> outputs);

}  // namespace forward_detail
}  // namespace radray

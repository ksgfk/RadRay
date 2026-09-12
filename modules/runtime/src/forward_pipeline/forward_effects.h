#pragma once

#include "forward_frame.h"
#include "forward_bindings.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>

namespace radray {
class RenderSystem;
namespace forward_detail {

class ForwardLitMeshPassProcessor;

class ForwardEffectTemplates {
public:
    struct Impl;
    ForwardEffectTemplates();
    ~ForwardEffectTemplates();
    Impl& Get() noexcept { return *_impl; }

private:
    unique_ptr<Impl> _impl;
};

struct ForwardEffectPrograms {
    array<Nullable<ShaderProgram*>, 15> Programs{};
    bool Initialize(RenderSystem& system);
};

struct ForwardShadowAtlas {
    RgTextureValue Texture;
    Nullable<const Forward_PassData*> Values{nullptr};
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
    struct LocalLight {
        float PositionRadius[4], ColorType[4], DirectionCosOuter[4], Cone[4];
    };
    ForwardViewDrawWork Main;
    array<ForwardViewDrawWork, 4> Cascades;
    DrawExecutionStats Execution;
    ViewCompletionToken Completion;
    bool ContentValid{false}, PassesSucceeded{true};
    bool TemporalHistory{false}, HistoryValid{false};
    vector<LocalLight> Lights;
    RgUploadData LightUpload;
    uint32_t LightCount{0};
    Forward_PassData ShadowValues{};
    void Reset();
};

bool DeclareForwardSharedShadows(RenderGraph& graph, ForwardEffectTemplates& templates, const ForwardPipelineSettings& settings, const ResolvedRenderView& primary,
                                 const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                                 ForwardHdrView& work, render::RenderBackend backend, bool& warned, ForwardShadowAtlas& out);

bool BuildForwardHdrView(RenderGraph& graph, ForwardEffectTemplates& templates, RenderPipelineContext& context, render::Device& device,
                         const ForwardEffectPrograms& programs, const ForwardPipelineSettings& settings,
                         const ResolvedRenderViewFamily& family, const ResolvedRenderView& sourceView,
                         const RenderSceneSnapshot& scene, const PackedCBufferTable& objects, FrameDrawResources& draws, ForwardBindingCache& bindings,
                         ForwardHdrView& work, bool firstOutputView, bool& lightOverflowWarned,
                         std::span<const ForwardOutputSurface> surfaces, std::span<RenderGraphOutputBinding> outputs,
                         const ForwardShadowAtlas& shadows, bool auxiliary, shared_ptr<ForwardLitMeshPassProcessor> sharedLit = {});

bool BuildForwardOutputOverlay(RenderGraph& graph, ForwardEffectTemplates& templates, RenderPipelineContext& context, const ForwardEffectPrograms& programs,
                               const ForwardOutputOverlay& overlay, render::RenderBackend backend, std::span<RenderGraphOutputBinding> outputs);

}  // namespace forward_detail
}  // namespace radray

#pragma once

#include <span>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_workload.h>
#include <radray/runtime/render_framework/view_state.h>
#include <radray/types.h>

namespace radray {

struct AppUpdateContext;

class ViewCompletionToken {
public:
    bool IsValid() const noexcept { return _graph != 0 && _view.IsValid(); }

private:
    friend class RenderPipelineContext;
    ViewStateId _view;
    uint64_t _graph{0}, _serial{0};
    uint32_t _index{UINT32_MAX};
};

struct RenderPrepareContext {
    const AppUpdateContext& App;
    std::span<const RenderOutputInfo> Outputs;
    RenderWorkloadBuilder& Workloads;
    vector<StreamingAssetRefAny>& RetainedAssets;
};

class RenderPipelineContext {
public:
    RenderPipelineContext(AppFrameContext& frame, RenderGraphFrameResources& graphResources, render::RenderPassRegistry& registry,
                          ViewStateRegistry& views, uint64_t serial, std::span<const ResolvedRenderViewFamily> families,
                          std::span<RenderSurfaceFrame> surfaces, RenderGraphExecutionReport& report);
    ~RenderPipelineContext();
    uint32_t FlightIndex() const noexcept;
    uint64_t FrameSerial() const noexcept { return _serial; }
    const render::RenderDeviceCapabilities& Capabilities() const noexcept;
    HostWriteBatch& HostWrites() const noexcept;
    std::span<const ResolvedRenderViewFamily> ViewFamilies() const noexcept { return _families; }
    RenderGraph CreateRenderGraph(std::string_view name);
    RgTextureHandle ImportOutput(RenderGraph& graph, RenderOutputId output);
    /// Explicit display composition may route scene output through a sampleable intermediate.
    bool SetOutputIntermediate(RenderGraph& graph, RenderOutputId output, RgTextureHandle texture);
    RgTextureHandle ImportOutputTarget(RenderGraph& graph, RenderOutputId output);
    std::span<const RenderSurfaceFrame> OutputSurfaces() const noexcept { return _surfaces; }
    RenderGraphExecutionResult ExecuteGraph(RenderGraph& graph);
    bool CommitView(ViewStateId view);
    ViewCompletionToken RegisterViewCompletion(RenderGraph& graph, ViewStateId view, RgPassHandle pass);
    bool CommitView(ViewStateId view, const ViewCompletionToken& completion, bool requiredDrawsSucceeded);
    void InvalidateView(ViewStateId view);
    bool PreparePrimitiveHistory(ResolvedRenderView& view, const RenderSceneSnapshot& snapshot);
    PrimitiveMotionData GetPrimitiveMotion(ViewStateId view, const RenderPrimitiveData& primitive) const noexcept;
    HistoryTexturePair AcquireHistoryTexture(const ResolvedRenderView& view, const ResolvedRenderViewFamily& family,
                                             const HistoryTextureRequest& request, string& reason);

private:
    struct ImportedOutput;
    AppFrameContext& _frame;
    RenderGraphFrameResources& _graphResources;
    render::RenderPassRegistry& _registry;
    ViewStateRegistry& _views;
    uint64_t _serial;
    std::span<const ResolvedRenderViewFamily> _families;
    std::span<RenderSurfaceFrame> _surfaces;
    RenderGraphExecutionReport& _report;
    vector<unique_ptr<ImportedOutput>> _imports;
    vector<std::pair<RenderOutputId, RgTextureHandle>> _intermediates;
    vector<HistoryTexturePair> _histories;
    struct ViewCompletion {
        ViewStateId View;
        RgPassHandle Pass;
        RgTextureHandle Output;
        bool Executed{false};
    };
    vector<ViewCompletion> _completions;
    vector<ViewStateId> _failedTemporalViews;
    uint64_t _graphGeneration{0};
    bool _executed{false}, _success{false};
};

class RenderPipeline {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// Game thread, after World::Tick and after this flight's previous GPU work has completed.
    /// Write only this flight's private input; append references needed until flight reuse.
    virtual void PrepareFrame(RenderPrepareContext& ctx);

    /// Render thread. Consume only this flight's immutable input and resolved families.
    virtual void Render(RenderPipelineContext& ctx) = 0;
};

}  // namespace radray

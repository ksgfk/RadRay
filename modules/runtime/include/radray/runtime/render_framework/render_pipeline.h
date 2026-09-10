#pragma once

#include <span>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_workload.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>
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
    RenderGraphRuntimeOptions RuntimeOptions{kPerformanceRenderGraphRuntimeOptions};
};

struct RenderGraphOutputBinding {
    RenderOutputId Output;
    RgTextureValue Texture;
};
Nullable<RenderGraphOutputBinding*> FindGraphOutput(std::span<RenderGraphOutputBinding> outputs, RenderOutputId id) noexcept;

class RenderPipelineContext {
public:
    RenderPipelineContext(AppFrameContext& frame, RenderGraphFrameResources& graphResources, render::RenderPassRegistry& registry,
                          ViewStateRegistry& views, uint64_t serial, std::span<const ResolvedRenderViewFamily> families,
                          std::span<RenderSurfaceFrame> surfaces, RenderGraphExecutionReport& report,
                          RenderGraphRuntimeOptions runtime = kDiagnosticRenderGraphRuntimeOptions);
    ~RenderPipelineContext();
    uint32_t FlightIndex() const noexcept;
    uint64_t FrameSerial() const noexcept { return _serial; }
    const render::RenderDeviceCapabilities& Capabilities() const noexcept;
    render::RenderBackend Backend() const noexcept;
    HostWriteBatch& HostWrites() const noexcept;
    std::span<const ResolvedRenderViewFamily> ViewFamilies() const noexcept { return _families; }
    const RenderGraphRuntimeOptions& GetRuntimeOptions() const noexcept { return _runtimeOptions; }
    RenderGraph CreateRenderGraph(std::string_view name);
    RgTextureValue ImportOutputTarget(RenderGraph& graph, RenderOutputId output);
    std::span<const RenderSurfaceFrame> OutputSurfaces() const noexcept { return _surfaces; }
    RenderGraphExecutionResult ExecuteGraph(RenderGraph& graph);
    bool CommitView(ViewStateId view);
    ViewCompletionToken RegisterViewCompletion(RenderGraph& graph, ViewStateId view, RgPassHandle pass, RgTextureValue output);
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
    RenderGraphRuntimeOptions _runtimeOptions{kDiagnosticRenderGraphRuntimeOptions};
    vector<shared_ptr<ImportedOutput>> _imports;
    shared_ptr<FrameSubmission> _submission;
    vector<HistoryTexturePair> _histories;
    vector<bool> _recordedHistories;
    struct ViewCompletion {
        ViewStateId View;
        RgPassHandle Pass;
        RgTextureValue Output;
        bool Executed{false};
        bool Consumed{false};
    };
    vector<ViewCompletion> _completions;
    vector<ViewStateId> _failedTemporalViews;
    vector<ViewStateId> _queuedViews;
    uint64_t _graphGeneration{0};
    bool _executed{false}, _success{false};
};

class RenderGraphComponent {
public:
    virtual ~RenderGraphComponent() noexcept = default;
    virtual void PrepareFrame(RenderPrepareContext&) {}
    /// Expand only into the supplied graph. Inputs/outputs are explicit content values.
    virtual void BuildGraph(RenderPipelineContext&, RenderGraph&, std::span<RenderGraphOutputBinding>) = 0;
    virtual void GraphRecorded(RenderPipelineContext&, const RenderGraph&, RenderGraphExecutionResult) {}
};

class RenderPipeline : public RenderGraphComponent {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// Game thread, after World::Tick and after this flight's previous GPU work has completed.
    /// Write only this flight's private input; append references needed until flight reuse.
    void PrepareFrame(RenderPrepareContext& ctx) override;

    /// Render thread. Consume only this flight's immutable input and resolved families.
    void BuildGraph(RenderPipelineContext& ctx, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override = 0;
};

}  // namespace radray

#pragma once

#include <span>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_workload.h>
#include <radray/runtime/render_framework/view_state.h>
#include <radray/types.h>

namespace radray {

struct AppUpdateContext;

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
    RenderGraphExecutionResult ExecuteGraph(RenderGraph& graph);
    bool CommitView(ViewStateId view);
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
    vector<HistoryTexturePair> _histories;
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

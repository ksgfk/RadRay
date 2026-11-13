#include <radray/runtime/render_graph_pipeline.h>

#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

class RenderGraphFramePipeline::SlotState {
public:
    SlotState(render::Device& device, uint64_t pageSize) noexcept : TransientPool(device), Uploads(device, pageSize) {}
    RDGTransientResourcePool TransientPool;
    UploadArena Uploads;
    Nullable<render::CommandBuffer*> Command{nullptr};
    Nullable<CompiledRDG*> CompiledGraph{nullptr};
};

RenderGraphFramePipeline::RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount, uint64_t uploadPageSize) : _constraints(device.GetRDGConstraints()) {
    if (slotCount == 0 || uploadPageSize == 0) RADRAY_ABORT("RenderGraphFramePipeline: slot count and upload page size must be positive");
    _slotStates.reserve(slotCount);
    for (uint32_t index = 0; index < slotCount; ++index) {
        _slotStates.push_back(make_unique<SlotState>(device.GetDevice(), uploadPageSize));
    }
}

RenderGraphFramePipeline::~RenderGraphFramePipeline() noexcept = default;

void RenderGraphFramePipeline::Extract(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Extract");
    if (slot.GetIndex() >= _slotStates.size()) RADRAY_ABORT("RenderGraphFramePipeline: invalid slot index");
    auto& state = *_slotStates[slot.GetIndex()];
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.ExtractFrameData");
        ExtractFrameData(slot);
    }
    RDGBuilder graph{slot, _constraints};
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.SetupGraph");
        SetupGraph(graph, slot);
    }
    state.CompiledGraph = graph.Compile();
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: RDG compile failed");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.ExtractMeshPass");
        RADRAY_PROFILE_PLOT("RadRay.FramePipeline.MeshPassId", static_cast<int64_t>(pass.Value));
        ExtractMeshPass(pass, slot, slot.GetPassDrawList(pass));
    }
}

void RenderGraphFramePipeline::ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList&) noexcept {
    RADRAY_ABORT("RenderGraphFramePipeline: alive mesh pass {} requires an ExtractMeshPass implementation", pass.Value);
}

void RenderGraphFramePipeline::Prepare(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Prepare");
    auto& state = *_slotStates[slot.GetIndex()];
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: no extracted graph");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) slot.GetPassDrawList(pass).SortVisibleCommands();
    state.TransientPool.BeginFrame();
    state.Uploads.Reset();
    if (!state.CompiledGraph->Prepare(slot, state.TransientPool, state.Uploads)) RADRAY_ABORT("RenderGraphFramePipeline: RDG Prepare failed");
    state.Command = slot.AcquireCommandBuffer();
    if (!state.Command) RADRAY_ABORT("RenderGraphFramePipeline: cannot acquire command buffer");
}

void RenderGraphFramePipeline::Record(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Record");
    auto& state = *_slotStates[slot.GetIndex()];
    if (!state.Command || !state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: graph not prepared");
    state.Command->Begin();
    if (!state.CompiledGraph->Record(*state.Command)) RADRAY_ABORT("RenderGraphFramePipeline: RDG Record failed");
    state.Command->End();
}

void RenderGraphFramePipeline::Submit(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Submit");
    auto& state = *_slotStates[slot.GetIndex()];
    SubmitFrame(slot, *state.Command);
    state.CompiledGraph->CommitSubmittedStates();
    state.CompiledGraph = nullptr;
    state.Command = nullptr;
}

}  // namespace radray

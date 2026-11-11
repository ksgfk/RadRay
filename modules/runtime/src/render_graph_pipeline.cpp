#include <radray/runtime/render_graph_pipeline.h>

#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

class RenderGraphFramePipeline::SlotState {
public:
    explicit SlotState(render::Device& device) noexcept : TransientPool(device) {}
    RDGTransientResourcePool TransientPool;
    Nullable<CompiledRDG*> CompiledGraph{nullptr};
};

RenderGraphFramePipeline::RenderGraphFramePipeline(DeviceContext& device, uint32_t slotCount) {
    if (slotCount == 0) RADRAY_ABORT("RenderGraphFramePipeline: slot count must be positive");
    _slotStates.reserve(slotCount);
    for (uint32_t index = 0; index < slotCount; ++index) {
        _slotStates.push_back(make_unique<SlotState>(device.GetDevice()));
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
    RDGBuilder graph{slot.GetArena()};
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

void RenderGraphFramePipeline::Record(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Record");
    if (slot.GetIndex() >= _slotStates.size()) RADRAY_ABORT("RenderGraphFramePipeline: invalid slot index");
    auto& state = *_slotStates[slot.GetIndex()];
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: no extracted graph");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) {
        slot.GetPassDrawList(pass).SortVisibleCommands();
    }
    state.TransientPool.BeginFrame();
    auto command = slot.AcquireCommandBuffer();
    if (!command) RADRAY_ABORT("RenderGraphFramePipeline: cannot acquire command buffer");
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.CommandBegin");
        command->Begin();
    }
    if (!state.CompiledGraph->Execute(slot, state.TransientPool, *command)) {
        RADRAY_ABORT("RenderGraphFramePipeline: RDG execute failed");
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.CommandEnd");
        command->End();
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.SubmitFrame");
        SubmitFrame(slot, *command);
    }
    state.CompiledGraph = nullptr;
}

}  // namespace radray

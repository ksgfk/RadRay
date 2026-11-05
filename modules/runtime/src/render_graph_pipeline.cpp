#include <radray/runtime/render_graph_pipeline.h>

#include <radray/runtime/gpu_device.h>

namespace radray {

struct RenderGraphFramePipeline::SlotState {
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

void RenderGraphFramePipeline::Extract(const Scene& source, FrameSlot& slot) noexcept {
    if (slot.GetIndex() >= _slotStates.size()) RADRAY_ABORT("RenderGraphFramePipeline: invalid slot index");
    auto& state = *_slotStates[slot.GetIndex()];
    ExtractFrameData(source, slot);
    RDGBuilder graph{slot.GetArena()};
    SetupGraph(graph, slot);
    state.CompiledGraph = graph.Compile();
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: RDG compile failed");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) {
        ExtractMeshPass(pass, source, slot, slot.GetPassDrawList(pass));
    }
}

void RenderGraphFramePipeline::Record(FrameSlot& slot) noexcept {
    if (slot.GetIndex() >= _slotStates.size()) RADRAY_ABORT("RenderGraphFramePipeline: invalid slot index");
    auto& state = *_slotStates[slot.GetIndex()];
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: no extracted graph");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) {
        slot.GetPassDrawList(pass).SortVisibleCommands();
    }
    state.TransientPool.BeginFrame();
    auto command = slot.AcquireCommandBuffer();
    if (!command) RADRAY_ABORT("RenderGraphFramePipeline: cannot acquire command buffer");
    command->Begin();
    if (!state.CompiledGraph->Execute(slot, state.TransientPool, *command)) {
        RADRAY_ABORT("RenderGraphFramePipeline: RDG execute failed");
    }
    command->End();
    SubmitFrame(slot, *command);
    state.CompiledGraph = nullptr;
}

}  // namespace radray

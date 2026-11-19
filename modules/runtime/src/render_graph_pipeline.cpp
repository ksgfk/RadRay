#include <radray/runtime/render_graph_pipeline.h>

#include <cmath>
#include <limits>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

class RenderGraphFramePipeline::SlotState {
public:
    SlotState(gpu::Device& device, uint64_t pageSize) noexcept : TransientPool(device), Uploads(device, pageSize) {}
    RDGTransientResourcePool TransientPool;
    UploadArena Uploads;
    Nullable<gpu::CommandBuffer*> Command{nullptr};
    Nullable<CompiledRDG*> CompiledGraph{nullptr};
    RDGGpuProfilingMode Profiling{RDGGpuProfilingMode::Off};
    uint64_t UserSequence{};
    uint32_t QueryCount{};
    unique_ptr<gpu::QueryPool> Queries;
    unique_ptr<gpu::Buffer> Readback;
    gpu::BufferState ReadbackState{gpu::BufferState::Undefined};
    RDGGpuTimingSnapshot Timing;
    bool TimingReady{};
    bool AbandonIfLost(FrameSlot& slot) noexcept;
    void PrepareTiming(FrameSlot& slot);
};

bool RenderGraphFramePipeline::SlotState::AbandonIfLost(FrameSlot& slot) noexcept {
    if (!slot.GetDevice().IsDeviceLost()) return false;
    CompiledGraph = nullptr;
    Command = nullptr;
    Profiling = RDGGpuProfilingMode::Off;
    QueryCount = 0;
    TimingReady = false;
    return true;
}

void RenderGraphFramePipeline::SlotState::PrepareTiming(FrameSlot& slot) {
    TimingReady = false;
    QueryCount = 0;
    Timing.Passes.clear();
    auto& device = slot.GetDevice();
    if (Profiling == RDGGpuProfilingMode::Off || !device.GetCapabilities().Features.TimestampQueries ||
        !slot.GetQueue().GetTimestampValidBits()) return;
    if (Profiling == RDGGpuProfilingMode::FrameAndPasses) {
        auto names = CompiledGraph->GetRecordedPassNames();
        if (names.size() > (std::numeric_limits<uint32_t>::max() - 2u) / 2u) return;
        Timing.Passes.reserve(names.size());
        for (auto& name : names) Timing.Passes.push_back({std::move(name), 0});
    }
    const auto count = 2u + 2u * static_cast<uint32_t>(Timing.Passes.size());
    if (!Queries || Queries->GetCount() < count) {
        auto queries = device.CreateQueryPool({gpu::QueryType::Timestamp, count, "RDG GPU timings"});
        auto buffer = device.CreateBuffer({.Size = uint64_t{count} * sizeof(uint64_t), .Memory = gpu::MemoryType::ReadBack,
                                           .Usage = gpu::BufferUse::CopyDestination | gpu::BufferUse::MapRead});
        if (!queries || !buffer) {
            RADRAY_WARN_LOG("RDG: cannot allocate timestamp resources for {} queries", count);
            return;
        }
        Queries = queries.Release();
        Readback = buffer.Release();
        ReadbackState = gpu::BufferState::Undefined;
    }
    Timing.FrameId = slot.GetFrameId();
    Timing.UserSequence = UserSequence;
    QueryCount = count;
}

void RenderGraphFramePipeline::SetGpuProfiling(FrameSlot& slot, RDGGpuProfilingMode mode, uint64_t sequence) noexcept {
    auto& state = *_slotStates.at(slot.GetIndex());
    state.Profiling = mode;
    state.UserSequence = sequence;
}

Nullable<const RDGGpuTimingSnapshot*> RenderGraphFramePipeline::GetCompletedGpuTiming(const FrameSlot& slot) const noexcept {
    const auto& state = *_slotStates.at(slot.GetIndex());
    return state.TimingReady ? &state.Timing : nullptr;
}

void RenderGraphFramePipeline::OnGpuTimingCompleted(FrameSlot&, const RDGGpuTimingSnapshot&) noexcept {}

void RenderGraphFramePipeline::OnSlotCompleted(FrameSlot& slot) noexcept {
    auto& state = *_slotStates[slot.GetIndex()];
    if (!state.QueryCount) return;
    const auto size = uint64_t{state.QueryCount} * sizeof(uint64_t);
    state.QueryCount = 0;
    const auto bits = slot.GetQueue().GetTimestampValidBits();
    const auto period = state.Queries->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs;
    if (!bits || bits > 64 || !std::isfinite(period) || period <= 0) return;
    const auto* ticks = static_cast<const uint64_t*>(state.Readback->Map(0, size));
    if (!ticks) {
        RADRAY_WARN_LOG("RDG: cannot map completed GPU timestamps");
        return;
    }
    state.Readback->InvalidateMappedRange({0, size});
    const uint64_t mask = std::numeric_limits<uint64_t>::max() >> (64 - bits);
    const auto elapsed = [&](size_t first) {
        const double ns = static_cast<double>((ticks[first + 1] - ticks[first]) & mask) * period;
        return ns >= static_cast<double>(std::numeric_limits<uint64_t>::max()) ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(ns);
    };
    state.Timing.TotalNanoseconds = elapsed(0);
    for (size_t i = 0; i < state.Timing.Passes.size(); ++i) state.Timing.Passes[i].Nanoseconds = elapsed(2 + 2 * i);
    state.Readback->Unmap();
    state.TimingReady = true;
    RADRAY_PROFILE_PLOT("RadRay.RDG.GpuNanoseconds", static_cast<double>(state.Timing.TotalNanoseconds));
    OnGpuTimingCompleted(slot, state.Timing);
}

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
    if (state.AbandonIfLost(slot)) return;
    if (!state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: no extracted graph");
    for (const auto pass : state.CompiledGraph->GetAliveMeshPasses()) slot.GetPassDrawList(pass).SortVisibleCommands();
    state.TransientPool.BeginFrame();
    state.Uploads.Reset();
    const bool prepared = state.CompiledGraph->Prepare(slot, state.TransientPool, state.Uploads);
    if (state.AbandonIfLost(slot)) return;
    if (!prepared) RADRAY_ABORT("RenderGraphFramePipeline: RDG Prepare failed");
    state.PrepareTiming(slot);
    if (state.AbandonIfLost(slot)) return;
    state.Command = slot.AcquireCommandBuffer();
    if (state.AbandonIfLost(slot)) return;
    if (!state.Command) RADRAY_ABORT("RenderGraphFramePipeline: cannot acquire command buffer");
}

void RenderGraphFramePipeline::Record(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Record");
    auto& state = *_slotStates[slot.GetIndex()];
    if (state.AbandonIfLost(slot)) return;
    if (!state.Command || !state.CompiledGraph) RADRAY_ABORT("RenderGraphFramePipeline: graph not prepared");
    state.Command->Begin();
    if (state.AbandonIfLost(slot)) return;
    if (state.QueryCount) {
        state.Command->ResetQueryPool(state.Queries.get(), 0, state.QueryCount);
        state.Command->WriteTimestamp({state.Queries.get(), gpu::QueryPipelineStage::Top, 0});
    }
    const bool recorded = state.CompiledGraph->Record(*state.Command, state.QueryCount > 2 ? state.Queries.get() : nullptr);
    if (!recorded && !slot.GetDevice().IsDeviceLost()) RADRAY_ABORT("RenderGraphFramePipeline: RDG Record failed");
    if (recorded && state.QueryCount && !slot.GetDevice().IsDeviceLost()) {
        state.Command->WriteTimestamp({state.Queries.get(), gpu::QueryPipelineStage::Bottom, 1});
        const gpu::ResourceBarrierDescriptor before = gpu::BarrierBufferDescriptor{state.Readback.get(), state.ReadbackState, gpu::BufferState::CopyDestination};
        state.Command->ResourceBarrier(std::span{&before, 1});
        state.Command->ResolveQueryData({state.Queries.get(), 0, state.QueryCount, state.Readback.get(), 0});
        const gpu::ResourceBarrierDescriptor after = gpu::BarrierBufferDescriptor{state.Readback.get(), gpu::BufferState::CopyDestination, gpu::BufferState::HostRead};
        state.Command->ResourceBarrier(std::span{&after, 1});
    }
    state.Command->End();
    state.AbandonIfLost(slot);
}

bool RenderGraphFramePipeline::Submit(FrameSlot& slot) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FramePipeline.Submit");
    auto& state = *_slotStates[slot.GetIndex()];
    const bool submitted = SubmitFrame(slot, *state.Command);
    if (submitted) {
        if (state.QueryCount) state.ReadbackState = gpu::BufferState::HostRead;
        state.CompiledGraph->CommitSubmittedStates();
    }
    state.Profiling = RDGGpuProfilingMode::Off;
    state.CompiledGraph = nullptr;
    state.Command = nullptr;
    return submitted;
}

}  // namespace radray

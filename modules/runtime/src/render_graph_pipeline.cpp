#include <radray/runtime/render_graph_pipeline.h>

#include <cmath>
#include <limits>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

class RenderGraphFramePipeline::SlotState {
public:
    SlotState(gpu::Device& device, uint64_t pageSize) noexcept : TransientPool(device), Staging(device, pageSize), Dynamic(device, pageSize) {}
    RDGTransientResourcePool TransientPool;
    StagingBufferPool Staging;
    DynamicBufferArena Dynamic;
    Nullable<gpu::CommandBuffer*> Command{nullptr};
    Nullable<CompiledRDG*> CompiledGraph{nullptr};
    class ProfilingResetGuard {
    public:
        explicit ProfilingResetGuard(SlotState& state) noexcept : Target(&state) {}
        ~ProfilingResetGuard() noexcept { if (Target) Target->ClearPending(); }
        Nullable<SlotState*> Target;
    };
    Nullable<ProfilingResetGuard*> PendingGuard{nullptr};
    void BindFrame(FrameSlot& slot) {
        if (!PendingGuard) PendingGuard = slot.Emplace<ProfilingResetGuard>(*this);
    }
    void ClearPending() noexcept {
        CompiledGraph = nullptr;
        Command = nullptr;
        if (PendingGuard) PendingGuard->Target = nullptr;
        PendingGuard = nullptr;
        PendingProfiling = RDGGpuProfilingMode::Off;
        PendingUserSequence = 0;
    }
    RDGGpuProfilingMode PendingProfiling{RDGGpuProfilingMode::Off};
    uint64_t PendingUserSequence{};
    uint32_t QueryCount{};
    unique_ptr<gpu::QueryPool> Queries;
    unique_ptr<gpu::Buffer> Readback;
    Nullable<const uint64_t*> MappedTicks{nullptr};
    ~SlotState() noexcept { ClearPending(); }
    gpu::BufferState ReadbackState{gpu::BufferState::Undefined};
    RDGGpuTimingSnapshot Timing;
    bool TimingReady{};
    bool AbandonIfLost(FrameSlot& slot) noexcept;
    void PrepareTiming(FrameSlot& slot);
    void ReadTiming(FrameSlot& slot) noexcept;
};

bool RenderGraphFramePipeline::SlotState::AbandonIfLost(FrameSlot& slot) noexcept {
    if (!slot.GetDevice().IsDeviceLost()) return false;
    CompiledGraph = nullptr;
    Command = nullptr;
    QueryCount = 0;
    TimingReady = false;
    return true;
}

void RenderGraphFramePipeline::SlotState::PrepareTiming(FrameSlot& slot) {
    TimingReady = false;
    QueryCount = 0;
    Timing.Passes.clear();
    Timing.TotalNanoseconds = 0;
    Timing.FrameId = slot.GetFrameId();

    Timing.Status = PendingProfiling == RDGGpuProfilingMode::Off ? RDGGpuTimingStatus::Off : RDGGpuTimingStatus::Unavailable;
    auto& device = slot.GetDevice();
    if (PendingProfiling == RDGGpuProfilingMode::Off || !device.GetCapabilities().Features.TimestampQueries ||
        !slot.GetQueue().GetTimestampValidBits()) return;
    if (PendingProfiling == RDGGpuProfilingMode::FrameAndPasses) {
        auto names = CompiledGraph->GetRecordedPassNames();
        if (names.size() > (std::numeric_limits<uint32_t>::max() - 2u) / 2u) return;
        Timing.Passes.reserve(names.size());
        for (auto& name : names) Timing.Passes.push_back({std::move(name), 0});
    }
    const auto count = 2u + 2u * static_cast<uint32_t>(Timing.Passes.size());
    if (!Queries || Queries->GetCount() < count) {
        auto queries = device.CreateQueryPool({gpu::QueryType::Timestamp, count, "RDG GPU timings"});
        auto buffer = device.CreateBuffer({.Size = uint64_t{count} * sizeof(uint64_t), .Memory = gpu::MemoryType::HostReadBack,
                                           .Usage = gpu::BufferUse::CopyDestination | gpu::BufferUse::MapRead});
        auto* ticks = queries && buffer ? static_cast<const uint64_t*>(buffer->GetMappedData().Get()) : nullptr;
        if (!ticks) {
            Timing.Status = RDGGpuTimingStatus::AllocationFailed;
            RADRAY_WARN_LOG("RDG: cannot allocate timestamp resources for {} queries", count);
            return;
        }
        MappedTicks = ticks;
        Queries = queries.Release();
        Readback = buffer.Release();
        ReadbackState = gpu::BufferState::Undefined;
    }
    QueryCount = count;
}

void RenderGraphFramePipeline::SetGpuProfiling(FrameSlot& slot, RDGGpuProfilingMode mode, uint64_t sequence) noexcept {
    auto& state = *_slotStates.at(slot.GetIndex());
    state.BindFrame(slot);
    state.PendingProfiling = mode;
    state.PendingUserSequence = sequence;
}

Nullable<const RDGGpuTimingSnapshot*> RenderGraphFramePipeline::GetCompletedGpuTiming(const FrameSlot& slot) const noexcept {
    const auto& state = *_slotStates.at(slot.GetIndex());
    return state.TimingReady ? &state.Timing : nullptr;
}

void RenderGraphFramePipeline::SlotState::ReadTiming(FrameSlot& slot) noexcept {
    if (!QueryCount) return;
    const auto size = uint64_t{QueryCount} * sizeof(uint64_t);
    QueryCount = 0;
    const auto bits = slot.GetQueue().GetTimestampValidBits();
    const auto period = Queries->GetTimestampCalibration(&slot.GetQueue()).TickPeriodNs;
    if (!bits || bits > 64 || !std::isfinite(period) || period <= 0) return;
    if (!Readback->InvalidateMappedRange({0, size})) return;
    const auto* ticks = MappedTicks.Get();
    const uint64_t mask = std::numeric_limits<uint64_t>::max() >> (64 - bits);
    const auto elapsed = [&](size_t first) {
        const double ns = static_cast<double>((ticks[first + 1] - ticks[first]) & mask) * period;
        return ns >= static_cast<double>(std::numeric_limits<uint64_t>::max()) ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(ns);
    };
    Timing.TotalNanoseconds = elapsed(0);
    for (size_t i = 0; i < Timing.Passes.size(); ++i) Timing.Passes[i].Nanoseconds = elapsed(2 + 2 * i);
    Timing.Status = RDGGpuTimingStatus::Valid;
    TimingReady = true;
    RADRAY_PROFILE_PLOT("RadRay.RDG.GpuNanoseconds", static_cast<double>(Timing.TotalNanoseconds));
}

void RenderGraphFramePipeline::OnFrameSlotRetired(FrameSlot&, const RDGGpuTimingSnapshot&) noexcept {}

void RenderGraphFramePipeline::OnSlotRetired(FrameSlot& slot, bool deviceLost) noexcept {
    auto& state = *_slotStates[slot.GetIndex()];
    if (!deviceLost) state.ReadTiming(slot);
    if (deviceLost || slot.GetDevice().IsDeviceLost()) {
        state.Timing.Status = RDGGpuTimingStatus::DeviceLost;
        state.TimingReady = false;
    }
    if (!state.TimingReady) {
        state.Timing.TotalNanoseconds = 0;
        state.Timing.Passes.clear();
    }
    state.QueryCount = 0;
    OnFrameSlotRetired(slot, state.Timing);
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
    state.BindFrame(slot);
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
    state.Staging.BeginFrame();
    state.Dynamic.BeginFrame();
    const bool prepared = state.CompiledGraph->Prepare(slot, state.TransientPool, state.Staging, state.Dynamic);
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
        state.Timing.UserSequence = state.PendingUserSequence;
        state.ClearPending();
    } else {
        state.CompiledGraph = nullptr;
        state.Command = nullptr;
    }
    return submitted;
}

}  // namespace radray

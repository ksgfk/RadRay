#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <cstring>

namespace radray {

namespace {

constexpr uint64_t kStagingBufferCopyAlignment = 4;

render::ResourceHints GetGpuAddressPageHints(render::Device* device) noexcept {
    return device != nullptr && device->GetBackend() == render::RenderBackend::D3D12
               ? render::ResourceHint::Dedicated
               : render::ResourceHint::None;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
//  FrameUploadScheduler
// ═══════════════════════════════════════════════════════════════

FrameUploadScheduler::~FrameUploadScheduler() noexcept = default;

task<FrameUploadScope> FrameUploadScheduler::BeginUpload() {
    stop_token stop = co_await CurrentStopToken();
    std::optional<FrameUploadScope> frame = co_await BeginFrameUploadAwaitable{this, stop};
    if (!frame.has_value()) {
        co_await StopCurrentTask();
        co_return FrameUploadScope{};
    }
    co_return frame.value();
}

FrameUploadRecord* FrameUploadScheduler::RegisterUpload(stop_token stop, std::coroutine_handle<> continuation) {
    FrameUploadRecord* record = _uploads.Enqueue(stop, continuation);
    record->Cmd = nullptr;
    record->Uploader = nullptr;
    record->FlightIndex = std::numeric_limits<uint32_t>::max();
    record->CurrentStage = FrameUploadStage::AwaitingFrame;
    return record;
}

bool FrameUploadScheduler::EraseUpload(FrameUploadRecord* record) noexcept {
    return _uploads.Erase(record);
}

bool FrameUploadScheduler::IsUploadAlive(FrameUploadRecord* record) const noexcept {
    return _uploads.IsAlive(record);
}

void FrameUploadScheduler::ResumeRecord(FrameUploadRecord* record) {
    _uploads.ResumeRecord(record);
}

void FrameUploadScheduler::CancelRecord(FrameUploadRecord* record) noexcept {
    _uploads.CancelRecord(record);
}

void FrameUploadScheduler::RunUploadPhase(
    render::CommandBuffer* cmdBuffer,
    ResourceUploader& uploader,
    uint32_t flightIndex) {
    vector<FrameUploadRecord*> pending;
    const size_t uploadCount = _uploads.Count();
    for (size_t i = 0; i < uploadCount; ++i) {
        FrameUploadRecord* record = _uploads.At(i);
        if (record != nullptr && record->CurrentStage == FrameUploadStage::AwaitingFrame) {
            pending.push_back(record);
        }
    }

    for (FrameUploadRecord* rec : pending) {
        if (rec->Canceled || rec->Stop.stop_requested()) {
            rec->Canceled = true;
            ResumeRecord(rec);
            if (IsUploadAlive(rec)) {
                EraseUpload(rec);
            }
            continue;
        }

        rec->Cmd = cmdBuffer;
        rec->Uploader = &uploader;
        rec->FlightIndex = flightIndex;
        rec->CurrentStage = FrameUploadStage::InFrame;

        ResumeRecord(rec);

        if (!IsUploadAlive(rec)) {
            continue;
        }
        if (rec->CurrentStage != FrameUploadStage::AwaitingFence) {
            EraseUpload(rec);
            continue;
        }
    }
}

void FrameUploadScheduler::NotifyFlightComplete(uint32_t flightIndex) {
    const size_t uploadCount = _uploads.Count();
    for (size_t i = 0; i < uploadCount; ++i) {
        FrameUploadRecord* rec = _uploads.At(i);
        if (rec == nullptr) {
            continue;
        }
        if (rec->CurrentStage == FrameUploadStage::AwaitingFence && rec->FlightIndex == flightIndex) {
            rec->CurrentStage = FrameUploadStage::FenceComplete;
        }
    }
}

void FrameUploadScheduler::PumpCompletedUploads() {
    bool resumedAny = true;
    while (resumedAny) {
        resumedAny = false;
        for (size_t i = 0; i < _uploads.Count();) {
            FrameUploadRecord* rec = _uploads.At(i);
            if (rec->Stop.stop_requested()) {
                rec->Canceled = true;
            }
            if (rec->Canceled || rec->CurrentStage == FrameUploadStage::FenceComplete) {
                ResumeRecord(rec);
                if (IsUploadAlive(rec)) {
                    EraseUpload(rec);
                }
                resumedAny = true;
                break;
            }
            ++i;
        }
    }
}

bool WaitFrameUploadGpuAwaitable::await_ready() noexcept {
    if (_record == nullptr) {
        return true;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
    }
    return _record->Canceled || _record->CurrentStage == FrameUploadStage::FenceComplete;
}

bool WaitFrameUploadGpuAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_record == nullptr) {
        return false;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
        return false;
    }
    _record->Continuation = h;
    _record->CurrentStage = FrameUploadStage::AwaitingFence;
    return true;
}

bool WaitFrameUploadGpuAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return false;
    }
    bool completed = !_record->Canceled && _record->CurrentStage == FrameUploadStage::FenceComplete;
    if (_scheduler != nullptr) {
        _scheduler->EraseUpload(_record);
    }
    _record = nullptr;
    return completed;
}

render::CommandBuffer* FrameUploadScope::GetCommandBuffer() const noexcept {
    return _record->Cmd;
}

ResourceUploader& FrameUploadScope::GetUploader() const noexcept {
    return *_record->Uploader;
}

uint32_t FrameUploadScope::GetFlightIndex() const noexcept {
    return _record->FlightIndex;
}

task<void> FrameUploadScope::WaitGpu() {
    bool completed = co_await WaitFrameUploadGpuAwaitable{_scheduler, _record};
    if (!completed) {
        co_await StopCurrentTask();
    }
}

bool BeginFrameUploadAwaitable::await_ready() const noexcept {
    return _scheduler == nullptr || _stop.stop_requested();
}

bool BeginFrameUploadAwaitable::await_suspend(std::coroutine_handle<> h) {
    if (_scheduler == nullptr || _stop.stop_requested()) {
        return false;
    }
    _record = _scheduler->RegisterUpload(_stop, h);
    return true;
}

std::optional<FrameUploadScope> BeginFrameUploadAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return std::nullopt;
    }
    if (_record->Canceled || _record->Stop.stop_requested()) {
        _record->Canceled = true;
        if (_scheduler != nullptr) {
            _scheduler->EraseUpload(_record);
        }
        _record = nullptr;
        return std::nullopt;
    }
    return FrameUploadScope{_scheduler, _record};
}

// ═══════════════════════════════════════════════════════════════
//  GpuSystem
// ═══════════════════════════════════════════════════════════════

void DeferredRenderDeleteQueue::Push(uint64_t targetFenceValue, unique_ptr<render::RenderBase> obj) noexcept {
    if (obj == nullptr) {
        return;
    }
    std::lock_guard lock{_mutex};
    _entries.emplace_back(DeferredRenderDeleteEntry{
        .TargetFenceValue = targetFenceValue,
        .Object = std::move(obj)});
}

void DeferredRenderDeleteQueue::Process(uint64_t completedFenceValue) noexcept {
    std::lock_guard lock{_mutex};
    std::erase_if(
        _entries,
        [completedFenceValue](const DeferredRenderDeleteEntry& entry) noexcept {
            return entry.TargetFenceValue <= completedFenceValue;
        });
}

void DeferredRenderDeleteQueue::Flush() noexcept {
    std::lock_guard lock{_mutex};
    _entries.clear();
}

uint32_t DeferredRenderDeleteQueue::Count() const noexcept {
    std::lock_guard lock{_mutex};
    return static_cast<uint32_t>(_entries.size());
}

bool GpuSystem::CompleteFlight(uint32_t flightIndex) {
    auto& flight = _flights[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    _lastFrameLatency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    flight.Signal = GpuSystem::FenceSignal::Invalid();
    if (_frameProfiler != nullptr) {
        _frameProfiler->Resolve(flightIndex);
    }
    _app->NotifyRenderComplete(AppRenderCompleteContext{.FlightIndex = flightIndex});
    flight.WaitForDestroy.clear();
    _uploader->CollectFlight(flightIndex);
    // 该 flight 的 fence 已完成:标记等在这个 flight 上的上传记录(供下次 scheduler pump 恢复加载协程)。
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->NotifyFlightComplete(flightIndex);
    }
    ProcessDeferredDeletes();
    return true;
}

bool GpuSystem::CompleteFlightIfReady(uint32_t flightIndex, bool wait) {
    if (flightIndex >= _flights.size()) {
        return false;
    }

    FlightSlot& flight = _flights[flightIndex];
    if (!flight.Signal.IsValid()) {
        return true;
    }
    if (wait) {
        flight.Signal.Fence->Wait(flight.Signal.Value);
    } else if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }
    return CompleteFlight(flightIndex);
}

void GpuSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }

    FlightSlot& flight = _flights[flightIndex];
    if (flight.RenderResources != nullptr) {
        flight.RenderResources->Reset();
    }
    flight.WaitForDestroy.clear();
    flight.FrameStartTime = std::chrono::steady_clock::now();
}

// ═════════════════════════════════════════════════════════════════
//  GpuFrameProfiler
// ═════════════════════════════════════════════════════════════════

GpuFrameProfiler::GpuFrameProfiler(render::Device* device, render::CommandQueue* queue, uint32_t flightCount)
    : _queue(queue) {
    // Vulkan 需要在 readback copy 前后显式 transition;D3D12 READBACK heap 始终处于 COPY_DEST。
    _readbackNeedsBarrier = device->GetBackend() == render::RenderBackend::Vulkan;
    _frames.resize(flightCount);
    for (FrameTiming& frame : _frames) {
        render::QueryPoolDescriptor poolDesc{
            .Type = render::QueryType::Timestamp,
            .Count = TimestampQueryCount,
            .DebugName = "GpuFrameProfiler Timestamp Pool"};
        frame.Pool = device->CreateQueryPool(poolDesc).Unwrap();

        render::BufferDescriptor readbackDesc{
            .Size = sizeof(uint64_t) * TimestampQueryCount,
            .Memory = render::MemoryType::ReadBack,
            .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead};
        if (device->GetBackend() == render::RenderBackend::Vulkan) {
            // VMA maps a complete backing block even for a 16-byte readback.
            readbackDesc.Hints = render::ResourceHint::Dedicated;
        }
        frame.Readback = device->CreateBuffer(readbackDesc).Unwrap();
    }
}

GpuFrameProfiler::~GpuFrameProfiler() noexcept = default;

void GpuFrameProfiler::BeginFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->ResetQueryPool(frame.Pool.get(), 0, TimestampQueryCount);
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Top,
        .Index = 0});
}

void GpuFrameProfiler::EndFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Bottom,
        .Index = 1});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toCopyDst = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::Common,
            .After = render::BufferState::CopyDestination};
        cmdBuffer->ResourceBarrier(std::span{&toCopyDst, 1});
    }
    cmdBuffer->ResolveQueryData(render::QueryResolveDescriptor{
        .Pool = frame.Pool.get(),
        .FirstIndex = 0,
        .Count = TimestampQueryCount,
        .Destination = frame.Readback.get(),
        .DestinationOffset = 0});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toHostRead = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::CopyDestination,
            .After = render::BufferState::HostRead};
        cmdBuffer->ResourceBarrier(std::span{&toHostRead, 1});
    }
    frame.Pending = true;
}

void GpuFrameProfiler::Resolve(uint32_t flightIndex) {
    if (flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    if (!frame.Pending) {
        return;
    }
    frame.Pending = false;

    const uint64_t mappedSize = sizeof(uint64_t) * TimestampQueryCount;
    void* mapped = frame.Readback->Map(0, mappedSize);
    if (mapped == nullptr) {
        return;
    }
    uint64_t ticks[TimestampQueryCount]{};
    std::memcpy(ticks, mapped, mappedSize);
    frame.Readback->Unmap(0, mappedSize);

    if (ticks[1] <= ticks[0]) {
        return;
    }
    const render::TimestampQueryCalibration calibration = frame.Pool->GetTimestampCalibration(_queue);
    if (calibration.TickPeriodNs <= 0.0) {
        return;
    }
    const double elapsedNs = static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs;
    _lastGpuTimeMs = static_cast<float>(elapsedNs / 1'000'000.0);
}

// ═════════════════════════════════════════════════════════════════
//  GpuSystem
// ═════════════════════════════════════════════════════════════════

GpuSystem::GpuSystem(Application* app, const GpuSystemDescriptor& desc)
    : _app(app),
      _device(desc.Device),
      _mainQueue(desc.Device->GetCommandQueue(render::QueueType::Direct, desc.MainQueueIndex).Unwrap()),
      _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
    _mainQueueTrack.Queue = _mainQueue;
    _mainQueueTrack.Fence = _device->CreateFence().Unwrap();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flights.resize(_flightDataCount);
    for (auto& flight : _flights) {
        flight.RenderResources = make_unique<FrameResources>(_device);
    }
    _uploader = make_unique<ResourceUploader>(_device, _flightDataCount);
    _frameUploadScheduler = make_unique<FrameUploadScheduler>();
    _pipelineLayoutLibrary = make_unique<PipelineLayoutLibrary>(_device);
    _renderPassRegistry = make_unique<RenderPassRegistry>(_device);
    if (desc.EnableFrameProfiler) {
        _frameProfiler = make_unique<GpuFrameProfiler>(_device, _mainQueue, _flightDataCount);
    }
}

GpuSystem::~GpuSystem() noexcept {
    FlushAllDeferredDeletes();
    // Binding groups and command buffers must go away before their cached
    // pipeline layouts and render-pass objects.
    _flights.clear();
    _renderPassRegistry.reset();
    _pipelineLayoutLibrary.reset();
}

void GpuSystem::RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept {
    if (obj == nullptr) {
        return;
    }
    const uint64_t targetFenceValue = _mainQueueTrack.NextFenceValue.load(std::memory_order_acquire);
    _deferredDeletes.Push(targetFenceValue, std::move(obj));
}

void GpuSystem::ProcessDeferredDeletes() noexcept {
    if (_mainQueueTrack.Fence == nullptr) {
        return;
    }
    _deferredDeletes.Process(_mainQueueTrack.Fence->GetCompletedValue());
}

void GpuSystem::FlushAllDeferredDeletes() noexcept {
    _deferredDeletes.Flush();
}

float GpuSystem::GetLastGpuTimeMs() const noexcept {
    return _frameProfiler != nullptr ? _frameProfiler->GetLastGpuTimeMs() : 0.0f;
}

uint32_t GpuSystem::GetCurrentFlightIndex() const noexcept {
    return static_cast<uint32_t>(_nowFrameIndex % _flightDataCount);
}

FrameResources& GpuSystem::GetFrameResources(uint32_t flightIndex) noexcept {
    return *_flights.at(flightIndex).RenderResources;
}

void GpuSystem::PumpFrameUploadScheduler() {
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->PumpCompletedUploads();
    }
}

void GpuSystem::WaitAndCleanupCompletedFlights() {
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        CompleteFlight(flightIndex);
    }
}

AppFrameContext GpuSystem::BeginFrameRecord(
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop) {
    FlightSlot& record = _flights[flightIndex];
    if (record.CmdBuffer == nullptr) {
        record.CmdBuffer = _device->CreateCommandBuffer(_mainQueue).Unwrap();
    }
    record.Targets.clear();
    record.ManualSubmit = false;
    record.Recording = true;
    record.CmdBuffer->Begin();
    // 帧顶(任何 RenderPass 之前、裸 CommandBuffer):交付本帧 cmd/uploader/flight 给等在
    // GPU 上传点的加载协程并 inline 恢复,让它们在本帧默认 cmdbuffer 上录制 copy。
    // copy 与本帧绘制同一提交,fence 完成后由 CompleteFlight 推进加载协程。
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->RunUploadPhase(record.CmdBuffer.get(), *_uploader, flightIndex);
    }
    if (_frameProfiler != nullptr) {
        _frameProfiler->BeginFrame(record.CmdBuffer.get(), flightIndex);
    }
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void GpuSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = _flights[flightIndex];
    if (!record.Recording) {
        return;
    }
    record.Recording = false;

    // 闭合上传链路：本帧录制的 staging + AssetRef 绑定到该 flight。
    _uploader->EndFlight(flightIndex);

    // 帧尾 GPU 耗时收尾:写 Bottom timestamp + resolve 到 readback(在 End 之前、后续 barrier 之后录制)。
    if (_frameProfiler != nullptr) {
        _frameProfiler->EndFrame(record.CmdBuffer.get(), flightIndex);
    }

    record.CmdBuffer->End();

    // ManualSubmit：应用已自行 submit/present，runtime 仅负责 End 与后续回收。
    if (record.ManualSubmit) {
        record.Targets.clear();
        return;
    }

    // 聚合全部 target 的同步对象。
    vector<render::SwapChainSyncObject*> waitToExecute;
    vector<render::SwapChainSyncObject*> readyToPresent;
    waitToExecute.reserve(record.Targets.size());
    readyToPresent.reserve(record.Targets.size());
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetWaitToDraw()) {
            waitToExecute.emplace_back(syncObject);
        }
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetReadyToPresent()) {
            readyToPresent.emplace_back(syncObject);
        }
    }

    render::Fence* frameFence = _mainQueueTrack.Fence.get();
    render::CommandBuffer* submitCmdBuffers[] = {record.CmdBuffer.get()};
    render::Fence* signalFences[] = {frameFence};
    uint64_t signalValues[] = {_mainQueueTrack.NextFenceValue.fetch_add(1, std::memory_order_acq_rel)};
    render::CommandQueueSubmitDescriptor submitDesc{
        .CmdBuffers = std::span{submitCmdBuffers},
        .SignalFences = std::span{signalFences},
        .SignalValues = std::span{signalValues},
        .WaitToExecute = std::span{waitToExecute},
        .ReadyToPresent = std::span{readyToPresent}};
    _mainQueue->Submit(submitDesc);
    _flights[flightIndex].Signal = GpuSystem::FenceSignal{
        .Fence = frameFence,
        .Value = signalValues[0]};

    // 逐个呈现。RequireRecreate 静默跳过，其余非 Success 记日志。
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        render::SwapChainPresentResult present =
            target.Window->PresentSwapChainFrame(std::move(target.Frame));
        if (present.Status == render::SwapChainStatus::RequireRecreate) {
            continue;
        }
        if (present.Status != render::SwapChainStatus::Success) {
            RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", present.Status, present.NativeStatusCode);
        }
    }
    record.Targets.clear();
}

// ══════════════════════════════════════════════
//  AppFrameContext
// ══════════════════════════════════════════════

render::CommandBuffer* AppFrameContext::GetCommandBuffer() const noexcept {
    return _gpuSystem->_flights[_flightIndex].CmdBuffer.get();
}

std::optional<AppFrameTarget> AppFrameContext::AcquireWindow(AppWindow* window) {
    if (window == nullptr) {
        return std::nullopt;
    }
    const AppRenderContext renderCtx{
        .FlightIndex = _flightIndex,
        .DeltaTime = _deltaTime,
        .LastFrameLatency = _lastFrameLatency,
        .IsInModalLoop = _isInModalLoop};
    render::SwapChainAcquireResult acquire = window->AcquireNextSwapChainFrame(renderCtx);
    if (acquire.Status != render::SwapChainStatus::Success || !acquire.Frame.has_value()) {
        if (acquire.Status != render::SwapChainStatus::RequireRecreate &&
            acquire.Status != render::SwapChainStatus::RetryLater) {
            RADRAY_ERR_LOG("failed to acquire swapchain frame: status={}, native={}", acquire.Status, acquire.NativeStatusCode);
        }
        return std::nullopt;
    }

    render::SwapChainFrame frame = std::move(acquire.Frame.value());
    render::Texture* backBuffer = frame.GetBackBuffer();
    const uint32_t backBufferIndex = frame.GetBackBufferIndex();
    render::TextureView* backBufferView = window->GetOrCreateBackBufferView(frame);
    if (backBufferView == nullptr) {
        // 未能建立 view：丢弃该 frame（不提交）。
        return std::nullopt;
    }

    GpuSystem::FlightSlot& record = _gpuSystem->_flights[_flightIndex];
    record.Targets.emplace_back(GpuFlightAcquiredTarget{
        .Window = window,
        .Frame = std::move(frame)});
    return AppFrameTarget{
        .Window = window,
        .BackBuffer = backBuffer,
        .BackBufferView = backBufferView,
        .BackBufferIndex = backBufferIndex};
}

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return *_gpuSystem->_uploader;
}

FrameResources& AppFrameContext::GetFrameResources() const noexcept {
    return _gpuSystem->GetFrameResources(_flightIndex);
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _gpuSystem->_device;
}

render::CommandQueue* AppFrameContext::GetMainQueue() const noexcept {
    return _gpuSystem->_mainQueue;
}

void AppFrameContext::SetManualSubmit() noexcept {
    _gpuSystem->_flights[_flightIndex].ManualSubmit = true;
}

// ═══════════════════════════════════════════════════════════════
//  StagingBufferPool
// ═══════════════════════════════════════════════════════════════

StagingBufferPool::StagingBufferPool(
    render::Device* device,
    uint32_t flightCount,
    const Descriptor& desc) noexcept
    : _device(device), _desc(desc) {
    if (_desc.PageSize == 0) {
        RADRAY_ABORT("StagingBufferPool page size must be non-zero");
    }
    _pending.resize(flightCount);
}

StagingBufferPool::StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept
    : StagingBufferPool(device, flightCount, Descriptor{}) {}

StagingBufferPool::~StagingBufferPool() noexcept = default;

StagingBufferPool::Page StagingBufferPool::CreatePage(uint64_t capacity, bool cacheable) {
    render::BufferDescriptor desc{
        .Size = capacity,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite,
        .Hints = render::ResourceHint::PersistentMap};
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        RADRAY_ABORT("StagingBufferPool failed to create upload buffer of size {}", capacity);
    }
    auto buffer = bufferOpt.Release();
    const std::string_view namePrefix = cacheable ? "staging_page" : "staging_large";
    buffer->SetDebugName(fmt::format("{}_{}", namePrefix, _nextPageId++));
    void* mapped = buffer->Map(0, capacity);
    if (mapped == nullptr) {
        RADRAY_ABORT("StagingBufferPool failed to map upload buffer of size {}", capacity);
    }
    return Page{
        .Buffer = std::move(buffer),
        .Mapped = mapped,
        .Used = 0,
        .Cacheable = cacheable};
}

StagingBufferPool::Page& StagingBufferPool::AcquireStandardPage() {
    if (!_freeList.empty()) {
        Page page = std::move(_freeList.back());
        _freeList.pop_back();
        page.Used = 0;
        _active.emplace_back(std::move(page));
    } else {
        _active.emplace_back(CreatePage(_desc.PageSize, true));
    }
    return _active.back();
}

bool StagingBufferPool::TryReserve(
    Page& page,
    uint64_t size,
    uint64_t alignment,
    uint64_t& offset) noexcept {
    if (page.Buffer == nullptr ||
        page.Used > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return false;
    }
    const uint64_t candidate = Align(page.Used, alignment);
    const uint64_t capacity = page.Buffer->GetDesc().Size;
    if (candidate > capacity || size > capacity - candidate) {
        return false;
    }
    offset = candidate;
    page.Used = candidate + size;
    return true;
}

StagingBufferPool::Allocation StagingBufferPool::Allocate(uint64_t size, uint64_t alignment) {
    if (size == 0) {
        return Allocation{};
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        RADRAY_ABORT("StagingBufferPool alignment {} must be a non-zero power of two", alignment);
    }
    if (_device == nullptr) {
        RADRAY_ABORT("StagingBufferPool cannot allocate without a render device");
    }

    uint64_t offset = 0;
    for (auto& page : _active) {
        if (!page.Cacheable || !TryReserve(page, size, alignment, offset)) {
            continue;
        }
        return Allocation{
            page.Buffer.get(),
            static_cast<byte*>(page.Mapped) + offset,
            offset,
            size};
    }

    Page* page = nullptr;
    if (size <= _desc.PageSize) {
        page = &AcquireStandardPage();
    } else {
        if (size > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
            RADRAY_ABORT("StagingBufferPool allocation size {} overflows alignment {}", size, alignment);
        }
        const uint64_t capacity = Align(size, alignment);
        _active.emplace_back(CreatePage(capacity, false));
        page = &_active.back();
    }
    if (!TryReserve(*page, size, alignment, offset)) {
        RADRAY_ABORT(
            "StagingBufferPool failed to reserve {} bytes with alignment {} from a {} byte page",
            size,
            alignment,
            page->Buffer->GetDesc().Size);
    }
    return Allocation{
        page->Buffer.get(),
        static_cast<byte*>(page->Mapped) + offset,
        offset,
        size};
}

void StagingBufferPool::Flush(const Allocation& allocation) {
    if (allocation.Buffer == nullptr || allocation.Size == 0) {
        return;
    }
    for (auto& page : _active) {
        if (page.Buffer.get() == allocation.Buffer) {
            page.Buffer->Unmap(allocation.Offset, allocation.Size);
            return;
        }
    }
}

void StagingBufferPool::RetireToFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _pending.size());
    auto& pending = _pending[flightIndex];
    pending.insert(
        pending.end(),
        std::make_move_iterator(_active.begin()),
        std::make_move_iterator(_active.end()));
    _active.clear();
}

void StagingBufferPool::CollectFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _pending.size());
    auto& pending = _pending[flightIndex];
    for (auto& page : pending) {
        if (page.Cacheable) {
            page.Used = 0;
            _freeList.emplace_back(std::move(page));
        }
    }
    pending.clear();
    TrimFreeList();
}

void StagingBufferPool::TrimFreeList() noexcept {
    std::erase_if(_freeList, [this](const Page& page) noexcept {
        return page.Buffer == nullptr ||
               !page.Cacheable ||
               page.Buffer->GetDesc().Size != _desc.PageSize;
    });

    uint64_t cachedBytes = 0;
    for (const auto& page : _freeList) {
        const uint64_t pageSize = page.Buffer->GetDesc().Size;
        cachedBytes = pageSize > std::numeric_limits<uint64_t>::max() - cachedBytes
                          ? std::numeric_limits<uint64_t>::max()
                          : cachedBytes + pageSize;
    }
    while (!_freeList.empty() &&
           (_freeList.size() > _desc.MaxCachedPages || cachedBytes > _desc.MaxCachedBytes)) {
        cachedBytes -= _freeList.back().Buffer->GetDesc().Size;
        _freeList.pop_back();
    }
}

// ═══════════════════════════════════════════════════════════════
//  ResourceUploader
// ═══════════════════════════════════════════════════════════════

ResourceUploader::ResourceUploader(render::Device* device, uint32_t flightCount)
    : _device(device),
      _stagingPool(device, flightCount) {
    (void)flightCount;
}

ResourceUploader::~ResourceUploader() noexcept = default;

void ResourceUploader::UploadBuffer(
    render::CommandBuffer* cmdBuffer,
    const BufferUploadRequest& request) {
    if (request.SrcData.empty() || request.DstBuffer == nullptr) {
        return;
    }
    const uint64_t size = request.SrcData.size();
    const auto dstDesc = request.DstBuffer->GetDesc();
    if (request.DstOffset > dstDesc.Size || size > dstDesc.Size - request.DstOffset) {
        return;
    }

    // 分配 staging 并拷贝 CPU 数据
    auto alloc = _stagingPool.Allocate(size, kStagingBufferCopyAlignment);
    std::memcpy(alloc.MappedPtr, request.SrcData.data(), size);
    _stagingPool.Flush(alloc);

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        barriersBefore.emplace_back(render::BarrierBufferDescriptor{
            .Target = alloc.Buffer,
            .Before = render::BufferState::HostWrite,
            .After = render::BufferState::CopySource});
    }
    barriersBefore.emplace_back(render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = request.Before,
        .After = render::BufferState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);

    // 录制 copy
    cmdBuffer->CopyBufferToBuffer(
        request.DstBuffer, request.DstOffset,
        alloc.Buffer, alloc.Offset,
        size);

    // 录制 barrier: dst → Common
    render::ResourceBarrierDescriptor barrierAfter = render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = render::BufferState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

// 计算单个 subresource 上传到 staging buffer 所需的字节数。
// 上传 buffer 按目标行距(dstRowPitch, 已对齐到 TextureDataPitchAlignment)
// 紧密堆叠每一行,共 mipHeight * mipDepth 行,这与两个后端 CopyBufferToTexture
// 消费的布局一致(vk: alignedBytesPerRow * mipHeight * mipDepth;
// d3d12: GetCopyableFootprints 的 totalBytes)。
// 返回 nullopt 表示参数非法或源数据不足。
static std::optional<uint64_t> GetSubresourceUploadSize(
    const render::TextureDescriptor& desc,
    const render::SubresourceRange& range,
    std::span<const byte> srcData,
    uint64_t srcRowPitch,
    uint64_t dstRowPitch) noexcept {
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0) {
        return std::nullopt;
    }
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t mipLevel = range.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t tightRowPitch = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    if (srcRowPitch < tightRowPitch || dstRowPitch < tightRowPitch) {
        return std::nullopt;
    }
    const uint64_t totalRows = static_cast<uint64_t>(mipHeight) * mipDepth;
    if (totalRows == 0) {
        return std::nullopt;
    }
    // 源数据最小字节数:前面每行步进 srcRowPitch,最后一行只需 tightRowPitch。
    const uint64_t requiredSrcSize = (totalRows - 1) * srcRowPitch + tightRowPitch;
    if (srcData.size() < requiredSrcSize) {
        return std::nullopt;
    }
    // 上传 buffer 字节数:每行按对齐后的 dstRowPitch 堆叠。
    return dstRowPitch * totalRows;
}

void ResourceUploader::UploadTexture(
    render::CommandBuffer* cmdBuffer,
    const TextureUploadRequest& request) {
    if (request.SrcData.empty() || request.DstTexture == nullptr) {
        return;
    }
    const auto desc = request.DstTexture->GetDesc();
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t arraySize = is3D ? 1u : desc.DepthOrArraySize;
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0 ||
        request.DstRange.MipLevelCount != 1 ||
        request.DstRange.ArrayLayerCount != 1 ||
        request.DstRange.BaseMipLevel >= desc.MipLevels ||
        request.DstRange.BaseArrayLayer >= arraySize) {
        return;
    }

    const uint32_t baseWidth = std::max(desc.Width >> request.DstRange.BaseMipLevel, 1u);
    const uint64_t tightRowPitch = static_cast<uint64_t>(baseWidth) * bytesPerPixel;
    const uint64_t srcRowPitch = request.SrcRowPitch == 0 ? tightRowPitch : request.SrcRowPitch;
    if (srcRowPitch < tightRowPitch) {
        return;
    }
    const uint64_t dstRowPitch = Align(tightRowPitch, std::max<uint64_t>(1, _device->GetDetail().TextureDataPitchAlignment));
    auto uploadSize = GetSubresourceUploadSize(desc, request.DstRange, request.SrcData, srcRowPitch, dstRowPitch);
    if (!uploadSize.has_value()) {
        return;
    }

    // 分配 staging 并拷贝 CPU 数据
    const uint64_t placementAlignment = std::max<uint64_t>(
        kStagingBufferCopyAlignment,
        _device->GetDetail().TextureDataPlacementAlignment);
    auto alloc = _stagingPool.Allocate(uploadSize.value(), placementAlignment);
    auto* dst = static_cast<byte*>(alloc.MappedPtr);
    const auto* src = request.SrcData.data();
    const uint32_t mipLevel = request.DstRange.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t rowBytes = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    uint64_t srcOffset = 0;
    uint64_t dstOffset = 0;
    for (uint32_t depth = 0; depth < mipDepth; ++depth) {
        for (uint32_t row = 0; row < mipHeight; ++row) {
            std::memcpy(dst + dstOffset, src + srcOffset, rowBytes);
            srcOffset += srcRowPitch;
            dstOffset += dstRowPitch;
        }
    }
    _stagingPool.Flush(alloc);

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        barriersBefore.emplace_back(render::BarrierBufferDescriptor{
            .Target = alloc.Buffer,
            .Before = render::BufferState::HostWrite,
            .After = render::BufferState::CopySource});
    }
    barriersBefore.emplace_back(render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = request.Before,
        .After = render::TextureState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);

    // 录制 copy
    cmdBuffer->CopyBufferToTexture(
        request.DstTexture, request.DstRange,
        alloc.Buffer, alloc.Offset);

    // 录制 barrier: dst → ShaderRead
    render::ResourceBarrierDescriptor barrierAfter = render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = render::TextureState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

void ResourceUploader::EndFlight(uint32_t flightIndex) {
    _stagingPool.RetireToFlight(flightIndex);
}

void ResourceUploader::CollectFlight(uint32_t flightIndex) {
    _stagingPool.CollectFlight(flightIndex);
}

std::optional<GpuMesh> ResourceUploader::UploadMeshResource(
    render::CommandBuffer* cmdBuffer,
    const MeshResource& meshResource) {
    if (meshResource.Primitives.empty()) {
        return std::nullopt;
    }

    GpuMesh result;
    vector<Nullable<render::Buffer*>> bufferByBin(meshResource.Bins.size());

    // 为每个 bin 创建 device-local buffer 并上传
    for (size_t binIdx = 0; binIdx < meshResource.Bins.size(); ++binIdx) {
        const MeshBuffer& bin = meshResource.Bins[binIdx];
        auto data = bin.GetData();
        if (data.empty()) {
            continue;
        }

        render::BufferDescriptor bufDesc{
            .Size = data.size(),
            .Memory = render::MemoryType::Device,
            .Usage = render::BufferUse::Vertex | render::BufferUse::Index | render::BufferUse::CopyDestination,
            .Hints = GetGpuAddressPageHints(_device)};
        auto bufOpt = _device->CreateBuffer(bufDesc);
        if (!bufOpt.HasValue()) {
            return std::nullopt;
        }
        auto buf = bufOpt.Release();
        buf->SetDebugName(fmt::format("{}_{}", meshResource.Name, binIdx));

        UploadBuffer(cmdBuffer, BufferUploadRequest{
                                    .SrcData = data,
                                    .DstBuffer = buf.get(),
                                    .DstOffset = 0,
                                    .Before = render::BufferState::Common,
                                    .After = render::BufferState::Vertex | render::BufferState::Index});

        bufferByBin[binIdx] = buf.get();
        result.Buffers.emplace_back(std::move(buf));
    }

    // 为每个 primitive 构建 DrawData
    for (size_t primIdx = 0; primIdx < meshResource.Primitives.size(); ++primIdx) {
        const MeshPrimitive& prim = meshResource.Primitives[primIdx];

        GpuMesh::DrawData drawData{};

        // 取第一个 vertex buffer entry 构建 VBV
        if (!prim.VertexBuffers.empty()) {
            const VertexBufferEntry& vbEntry = prim.VertexBuffers[0];
            if (vbEntry.BufferIndex < bufferByBin.size() && bufferByBin[vbEntry.BufferIndex].HasValue()) {
                uint64_t vbSize = static_cast<uint64_t>(prim.VertexCount) * vbEntry.Stride;
                drawData.Vbv = render::VertexBufferView{
                    .Target = bufferByBin[vbEntry.BufferIndex].Get(),
                    .Offset = vbEntry.Offset,
                    .Size = vbSize};
            }
        }

        // 构建 IBV
        if (prim.IndexBuffer.BufferIndex < bufferByBin.size() && bufferByBin[prim.IndexBuffer.BufferIndex].HasValue()) {
            drawData.Ibv = render::IndexBufferView{
                .Target = bufferByBin[prim.IndexBuffer.BufferIndex].Get(),
                .Offset = prim.IndexBuffer.Offset,
                .Stride = prim.IndexBuffer.Stride};
        }

        result.Draws.emplace_back(drawData);
    }

    return result;
}

}  // namespace radray

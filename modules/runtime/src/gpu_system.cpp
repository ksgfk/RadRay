#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#include <cstring>
#include <type_traits>

namespace radray {

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
    flight.HostWrites.Reset();
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
    ScopedBufferMap mapping{
        frame.Readback.get(),
        render::BufferRange{.Offset = 0, .Size = mappedSize}};
    if (!mapping) {
        return;
    }
    uint64_t ticks[TimestampQueryCount]{};
    std::memcpy(ticks, mapping.Data(), mappedSize);

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
      _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
    render::DeviceDescriptor deviceDesc = desc.Device;
    std::visit(
        [this, &desc](auto& backendDesc) {
            using BackendDescriptor = std::remove_cvref_t<decltype(backendDesc)>;
            if constexpr (std::is_same_v<BackendDescriptor, render::VulkanDeviceDescriptor>) {
                _vulkanInstance = render::InstanceVulkan::InitEnv(desc.VulkanInstance).Unwrap();
            } else if constexpr (std::is_same_v<BackendDescriptor, render::D3D12DeviceDescriptor>) {
                if (backendDesc.Factory != nullptr) {
                    RADRAY_ABORT("GpuSystem owns the DXGI factory; D3D12DeviceDescriptor::Factory must be null");
                }
                _dxgiFactory = render::DXGIFactory::Create(desc.DXGIFactory).Unwrap();
                backendDesc.Factory = _dxgiFactory.get();
            } else {
                RADRAY_ABORT("unsupported render backend");
            }
        },
        deviceDesc);
    _device = render::Device::Create(deviceDesc).Unwrap();

    _mainQueue = _device->GetCommandQueue(render::QueueType::Direct, desc.MainQueueIndex).Unwrap();
    _mainQueueTrack.Queue = _mainQueue;
    _mainQueueTrack.Fence = _device->CreateFence().Unwrap();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flights.resize(_flightDataCount);
    for (auto& flight : _flights) {
        flight.RenderResources = make_unique<FrameResources>(_device.get(), &flight.HostWrites);
    }
    _uploader = make_unique<ResourceUploader>(_device.get(), _flightDataCount);
    _frameUploadScheduler = make_unique<FrameUploadScheduler>();
    _pipelineLayoutLibrary = make_unique<PipelineLayoutLibrary>(_device.get());
    _renderPassRegistry = make_unique<RenderPassRegistry>(_device.get());
    if (desc.EnableFrameProfiler) {
        _frameProfiler = make_unique<GpuFrameProfiler>(_device.get(), _mainQueue, _flightDataCount);
    }
}

GpuSystem::~GpuSystem() noexcept {
    FlushAllDeferredDeletes();
    // Binding groups and command buffers must go away before their cached
    // pipeline layouts and render-pass objects.
    _flights.clear();
    _frameUploadScheduler.reset();
    _frameProfiler.reset();
    _uploader.reset();
    _renderPassRegistry.reset();
    _pipelineLayoutLibrary.reset();
    _mainQueueTrack.Fence.reset();
    _mainQueueTrack.Queue = nullptr;
    _mainQueue = nullptr;
    _device.reset();
    _dxgiFactory.reset();
    if (_vulkanInstance != nullptr) {
        render::InstanceVulkan::ShutdownEnv();
        _vulkanInstance = nullptr;
    }
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

UploadMemoryStats GpuSystem::GetUploadMemoryStats() const noexcept {
    UploadMemoryStats result{};
    for (const FlightSlot& flight : _flights) {
        const UploadMemoryStats& stats = flight.HostWrites.GetStats();
        result.PageCount += stats.PageCount;
        result.PageCapacityBytes += stats.PageCapacityBytes;
        result.CommitCount += stats.CommitCount;
        result.CommittedBytes += stats.CommittedBytes;
        result.RecordedRangeCount += stats.RecordedRangeCount;
        result.FlushedRangeCount += stats.FlushedRangeCount;
    }
    return result;
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
    record.Submitted = false;
    record.Recording = true;
    record.CmdBuffer->Begin();
    _uploader->BeginFlight(flightIndex, record.HostWrites);
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
    if (record.Submitted || !record.Recording) {
        return;
    }
    SubmitFrame(flightIndex, {});
}

void GpuSystem::SubmitFrame(
    uint32_t flightIndex,
    const AppFrameSubmitDescriptor& desc) {
    FlightSlot& record = _flights.at(flightIndex);
    if (!record.Recording || record.Submitted) {
        RADRAY_ABORT("GpuSystem::SubmitFrame called outside an active frame");
    }
    if (desc.SignalFences.size() != desc.SignalValues.size() ||
        desc.WaitFences.size() != desc.WaitValues.size()) {
        RADRAY_ABORT("AppFrameSubmitDescriptor fence/value counts do not match");
    }
    record.Recording = false;

    // 闭合上传链路：本帧录制的 staging + AssetRef 绑定到该 flight。
    _uploader->EndFlight(flightIndex);

    // 帧尾 GPU 耗时收尾:写 Bottom timestamp + resolve 到 readback(在 End 之前、后续 barrier 之后录制)。
    if (_frameProfiler != nullptr) {
        _frameProfiler->EndFrame(record.CmdBuffer.get(), flightIndex);
    }

    record.CmdBuffer->End();

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
    vector<render::CommandBuffer*> submitCmdBuffers;
    submitCmdBuffers.reserve(desc.CmdBuffers.size() + 1);
    submitCmdBuffers.push_back(record.CmdBuffer.get());
    submitCmdBuffers.insert(submitCmdBuffers.end(), desc.CmdBuffers.begin(), desc.CmdBuffers.end());

    vector<render::Fence*> signalFences;
    vector<uint64_t> signalValues;
    signalFences.reserve(desc.SignalFences.size() + 1);
    signalValues.reserve(desc.SignalValues.size() + 1);
    signalFences.push_back(frameFence);
    const uint64_t frameFenceValue = _mainQueueTrack.NextFenceValue.fetch_add(1, std::memory_order_acq_rel);
    signalValues.push_back(frameFenceValue);
    signalFences.insert(signalFences.end(), desc.SignalFences.begin(), desc.SignalFences.end());
    signalValues.insert(signalValues.end(), desc.SignalValues.begin(), desc.SignalValues.end());

    render::CommandQueueSubmitDescriptor submitDesc{
        .CmdBuffers = submitCmdBuffers,
        .SignalFences = signalFences,
        .SignalValues = signalValues,
        .WaitFences = desc.WaitFences,
        .WaitValues = desc.WaitValues,
        .WaitToExecute = std::span{waitToExecute},
        .ReadyToPresent = std::span{readyToPresent}};
    record.HostWrites.Flush(*_device);
    _mainQueue->Submit(submitDesc);
    record.HostWrites.Seal();
    _flights[flightIndex].Signal = GpuSystem::FenceSignal{
        .Fence = frameFence,
        .Value = frameFenceValue};

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
    record.Submitted = true;
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
    return _gpuSystem->_device.get();
}

void AppFrameContext::SubmitFrame(const AppFrameSubmitDescriptor& desc) {
    _gpuSystem->SubmitFrame(_flightIndex, desc);
}

}  // namespace radray
